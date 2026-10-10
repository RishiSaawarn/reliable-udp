// Thin RAII wrappers over POSIX UDP sockets. IPv4 only, which is all the
// examples and benchmarks need; see docs/PROTOCOL.md "Limitations".
#pragma once
#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "reliable_udp/datagram_sink.hpp"

namespace rudp::net {

class SocketError : public std::runtime_error {
 public:
  SocketError(const std::string& what, int err);
  int error() const { return err_; }

 private:
  int err_;
};

struct Address {
  sockaddr_in sa{};

  Address() { sa.sin_family = AF_INET; }
  static Address parse(const std::string& host, std::uint16_t port);
  static Address loopback(std::uint16_t port);
  static Address any(std::uint16_t port);
  std::string str() const;
  std::uint16_t port() const;
  bool operator==(const Address& o) const {
    return sa.sin_addr.s_addr == o.sa.sin_addr.s_addr && sa.sin_port == o.sa.sin_port;
  }
  // Stable 64-bit identity, used as part of the server's session table key.
  std::uint64_t key() const {
    return (static_cast<std::uint64_t>(sa.sin_addr.s_addr) << 16) |
           static_cast<std::uint64_t>(sa.sin_port);
  }
};

// Monotonic microseconds. Never CLOCK_REALTIME: the protocol timers must not
// move when the system clock is stepped.
std::uint64_t now_us();

class UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(UdpSocket&& o) noexcept;
  UdpSocket& operator=(UdpSocket&& o) noexcept;

  void bind_to(const Address& a);
  void set_nonblocking(bool on);
  void set_rcvbuf(int bytes);
  void set_sndbuf(int bytes);
  int rcvbuf() const;
  Address local_address() const;
  int fd() const { return fd_; }

  // Two-iovec gather send. A datagram socket is all-or-nothing, so a short
  // write is a hard error rather than something to resume; it is reported.
  // Returns false when the kernel buffer is full (EAGAIN) or the datagram was
  // truncated; true on success. EINTR is retried internally.
  bool send_gather(const std::uint8_t* head, std::size_t head_len,
                   const std::uint8_t* payload, std::size_t payload_len, const Address* to);

  // -1 = nothing available (EAGAIN), otherwise the datagram length.
  std::ptrdiff_t recv_from(std::uint8_t* buf, std::size_t cap, Address* from);

  // Waits up to timeout_us for readability. Returns true if readable.
  bool wait_readable(std::uint64_t timeout_us);

  std::uint64_t send_errors() const { return send_errors_; }
  std::uint64_t send_would_block() const { return send_would_block_; }
  std::uint64_t truncated_sends() const { return truncated_sends_; }

 private:
  int fd_ = -1;
  std::uint64_t send_errors_ = 0;
  std::uint64_t send_would_block_ = 0;
  std::uint64_t truncated_sends_ = 0;
};

// Adapts a socket + fixed destination to the engine's DatagramSink.
class UdpSink final : public DatagramSink {
 public:
  UdpSink(UdpSocket& s, const Address& to) : sock_(&s), to_(to) {}
  void set_destination(const Address& to) { to_ = to; }
  const Address& destination() const { return to_; }
  void send_datagram(const std::uint8_t* head, std::size_t head_len,
                     const std::uint8_t* payload, std::size_t payload_len) override {
    if (!sock_->send_gather(head, head_len, payload, payload_len, &to_)) ++failed_;
    else ++sent_;
  }
  std::uint64_t sent() const { return sent_; }
  std::uint64_t failed() const { return failed_; }

 private:
  UdpSocket* sock_;
  Address to_;
  std::uint64_t sent_ = 0;
  std::uint64_t failed_ = 0;
};

}  // namespace rudp::net
