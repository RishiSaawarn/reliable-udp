#include "reliable_udp/net/udp_socket.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>

namespace rudp::net {

SocketError::SocketError(const std::string& what, int err)
    : std::runtime_error(what + ": " + std::string(std::strerror(err))), err_(err) {}

std::uint64_t now_us() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ull +
         static_cast<std::uint64_t>(ts.tv_nsec) / 1000ull;
}

Address Address::parse(const std::string& host, std::uint16_t port) {
  Address a;
  a.sa.sin_family = AF_INET;
  a.sa.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &a.sa.sin_addr) != 1) {
    throw SocketError("inet_pton(" + host + ")", EINVAL);
  }
  return a;
}
Address Address::loopback(std::uint16_t port) { return parse("127.0.0.1", port); }
Address Address::any(std::uint16_t port) {
  Address a;
  a.sa.sin_family = AF_INET;
  a.sa.sin_port = htons(port);
  a.sa.sin_addr.s_addr = htonl(INADDR_ANY);
  return a;
}
std::uint16_t Address::port() const { return ntohs(sa.sin_port); }
std::string Address::str() const {
  char b[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, &sa.sin_addr, b, sizeof(b));
  char out[64];
  std::snprintf(out, sizeof(out), "%s:%u", b, static_cast<unsigned>(port()));
  return std::string(out);
}

UdpSocket::UdpSocket() {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) throw SocketError("socket", errno);
}

UdpSocket::~UdpSocket() {
  if (fd_ >= 0) {
    // EINTR from close() must not be retried on Linux: the descriptor is gone.
    ::close(fd_);
  }
}

UdpSocket::UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
  if (this != &o) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = o.fd_;
    o.fd_ = -1;
  }
  return *this;
}

void UdpSocket::bind_to(const Address& a) {
  if (::bind(fd_, reinterpret_cast<const sockaddr*>(&a.sa), sizeof(a.sa)) != 0) {
    throw SocketError("bind(" + a.str() + ")", errno);
  }
}

void UdpSocket::set_nonblocking(bool on) {
  int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0) throw SocketError("fcntl(F_GETFL)", errno);
  flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (::fcntl(fd_, F_SETFL, flags) != 0) throw SocketError("fcntl(F_SETFL)", errno);
}

void UdpSocket::set_rcvbuf(int bytes) {
  if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) != 0) {
    throw SocketError("setsockopt(SO_RCVBUF)", errno);
  }
}
void UdpSocket::set_sndbuf(int bytes) {
  if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) != 0) {
    throw SocketError("setsockopt(SO_SNDBUF)", errno);
  }
}
int UdpSocket::rcvbuf() const {
  int v = 0;
  socklen_t l = sizeof(v);
  if (::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &v, &l) != 0) return -1;
  return v;
}

Address UdpSocket::local_address() const {
  Address a;
  socklen_t l = sizeof(a.sa);
  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&a.sa), &l) != 0) {
    throw SocketError("getsockname", errno);
  }
  return a;
}

bool UdpSocket::send_gather(const std::uint8_t* head, std::size_t head_len,
                            const std::uint8_t* payload, std::size_t payload_len,
                            const Address* to) {
  iovec iov[2];
  iov[0].iov_base = const_cast<std::uint8_t*>(head);
  iov[0].iov_len = head_len;
  int niov = 1;
  if (payload_len > 0) {
    iov[1].iov_base = const_cast<std::uint8_t*>(payload);
    iov[1].iov_len = payload_len;
    niov = 2;
  }
  msghdr msg{};
  if (to != nullptr) {
    msg.msg_name = const_cast<sockaddr_in*>(&to->sa);
    msg.msg_namelen = sizeof(to->sa);
  }
  msg.msg_iov = iov;
  msg.msg_iovlen = static_cast<std::size_t>(niov);

  for (;;) {
    const ssize_t n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
    if (n >= 0) {
      if (static_cast<std::size_t>(n) != head_len + payload_len) {
        // Cannot happen for SOCK_DGRAM, but a short write would silently
        // corrupt the stream, so it is detected rather than assumed away.
        ++truncated_sends_;
        return false;
      }
      return true;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
      ++send_would_block_;
      return false;
    }
    ++send_errors_;
    return false;
  }
}

std::ptrdiff_t UdpSocket::recv_from(std::uint8_t* buf, std::size_t cap, Address* from) {
  for (;;) {
    sockaddr_in src{};
    socklen_t sl = sizeof(src);
    const ssize_t n = ::recvfrom(fd_, buf, cap, 0, reinterpret_cast<sockaddr*>(&src), &sl);
    if (n >= 0) {
      if (from != nullptr) from->sa = src;
      return n;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
    if (errno == ECONNREFUSED) return -1;  // stale ICMP from a previous peer
    throw SocketError("recvfrom", errno);
  }
}

bool UdpSocket::wait_readable(std::uint64_t timeout_us) {
  pollfd p{};
  p.fd = fd_;
  p.events = POLLIN;
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(timeout_us / 1000000ull);
  ts.tv_nsec = static_cast<long>((timeout_us % 1000000ull) * 1000ull);
  for (;;) {
    // ppoll gives sub-millisecond timeouts, which matters because the RTO
    // floor is tens of microseconds on loopback.
    const int r = ::ppoll(&p, 1, &ts, nullptr);
    if (r > 0) return (p.revents & POLLIN) != 0;
    if (r == 0) return false;
    if (errno == EINTR) continue;
    throw SocketError("ppoll", errno);
  }
}

}  // namespace rudp::net
