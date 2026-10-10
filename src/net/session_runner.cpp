#include "reliable_udp/net/session_runner.hpp"

#include <algorithm>

#include "reliable_udp/receiver.hpp"
#include "reliable_udp/sender.hpp"
#include "reliable_udp/sha256.hpp"

namespace rudp::net {
namespace {

RttSnapshot snapshot(const RttEstimator& r) {
  RttSnapshot s;
  s.samples = r.samples();
  s.srtt_us = r.srtt_us();
  s.rttvar_us = r.rttvar_us();
  s.mean_us = r.mean_rtt_us();
  s.min_us = r.min_rtt_us();
  s.max_us = r.max_rtt_us();
  s.final_rto_us = r.rto_us();
  s.base_rto_us = r.base_rto_us();
  s.backoffs = r.timeouts();
  return s;
}

std::uint64_t poll_timeout(std::uint64_t deadline, std::uint64_t now, std::uint64_t slice) {
  if (deadline == UINT64_MAX) return slice;
  if (deadline <= now) return 0;
  return std::min(deadline - now, slice);
}

}  // namespace

RunResult send_stream(const Address& peer, const std::uint8_t* data, std::size_t len,
                      const RunnerConfig& rc, const RecvFilter& filter) {
  RunResult res;
  try {
    UdpSocket sock;
    sock.set_nonblocking(true);
    sock.set_rcvbuf(rc.rcvbuf_bytes);
    sock.set_sndbuf(rc.sndbuf_bytes);
    res.socket_rcvbuf = static_cast<std::uint64_t>(std::max(0, sock.rcvbuf()));
    UdpSink sink(sock, peer);
    SrSender sender(rc.proto, sink, data, len);

    // One stack buffer for the whole session: the receive path never allocates.
    std::vector<std::uint8_t> rxbuf(kMaxDatagramSize);
    const std::uint64_t t0 = now_us();
    std::uint64_t last_progress = t0;
    sender.start(t0);

    while (!sender.done() && !sender.failed()) {
      const std::uint64_t now = now_us();
      const std::uint64_t to = poll_timeout(sender.next_deadline_us(), now, rc.poll_slice_us);
      if (sock.wait_readable(to)) {
        ++res.poll_wakeups;
        for (std::uint32_t i = 0; i < rc.max_burst_reads; ++i) {
          Address from;
          const std::ptrdiff_t n = sock.recv_from(rxbuf.data(), rxbuf.size(), &from);
          if (n < 0) break;
          ++res.rx_datagrams;
          res.rx_bytes += static_cast<std::uint64_t>(n);
          if (filter && !filter(rxbuf.data(), static_cast<std::size_t>(n))) {
            ++res.filtered_out;
            continue;
          }
          sender.on_datagram(rxbuf.data(), static_cast<std::size_t>(n), now_us());
        }
        last_progress = now_us();
      }
      sender.tick(now_us());
      if (now_us() - last_progress > rc.idle_timeout_us) {
        res.error = "idle timeout waiting for peer";
        break;
      }
    }

    res.wall_us = now_us() - t0;
    res.snd = sender.stats();
    res.rtt = snapshot(sender.rtt());
    res.tx_datagrams = sink.sent();
    res.tx_failed = sink.failed() + sock.send_errors() + sock.truncated_sends();
    res.bytes = len;
    res.sha256 = sha256_hex(data, len);
    if (sender.failed()) {
      res.error = sender.failure_reason();
    } else if (res.error.empty()) {
      res.ok = sender.done();
    }
  } catch (const std::exception& e) {
    res.error = e.what();
  }
  return res;
}

RunResult recv_stream_on(UdpSocket& sock, std::vector<std::uint8_t>* out,
                         const RunnerConfig& rc, const RecvFilter& filter) {
  RunResult res;
  try {
    sock.set_nonblocking(true);
    sock.set_rcvbuf(rc.rcvbuf_bytes);
    sock.set_sndbuf(rc.sndbuf_bytes);
    res.socket_rcvbuf = static_cast<std::uint64_t>(std::max(0, sock.rcvbuf()));

    Address peer;
    bool have_peer = false;
    UdpSink sink(sock, peer);
    Sha256 hasher;
    std::size_t total = 0;
    SrReceiver receiver(rc.proto, sink, [&](const std::uint8_t* p, std::size_t n) {
      hasher.update(p, n);
      total += n;
      if (out) out->insert(out->end(), p, p + n);
    });

    std::vector<std::uint8_t> rxbuf(kMaxDatagramSize);
    const std::uint64_t t0 = now_us();
    std::uint64_t last_progress = t0;

    while (!receiver.done()) {
      const std::uint64_t now = now_us();
      const std::uint64_t to = poll_timeout(receiver.next_deadline_us(), now, rc.poll_slice_us);
      if (sock.wait_readable(to)) {
        ++res.poll_wakeups;
        for (std::uint32_t i = 0; i < rc.max_burst_reads; ++i) {
          Address from;
          const std::ptrdiff_t n = sock.recv_from(rxbuf.data(), rxbuf.size(), &from);
          if (n < 0) break;
          ++res.rx_datagrams;
          res.rx_bytes += static_cast<std::uint64_t>(n);
          if (filter && !filter(rxbuf.data(), static_cast<std::size_t>(n))) {
            ++res.filtered_out;
            continue;
          }
          if (!have_peer) {
            peer = from;
            have_peer = true;
            sink.set_destination(peer);
          } else if (!(from == peer)) {
            continue;  // datagram from a third party: not our session
          }
          receiver.on_datagram(rxbuf.data(), static_cast<std::size_t>(n), now_us());
        }
        last_progress = now_us();
      }
      receiver.tick(now_us());
      if (now_us() - last_progress > rc.idle_timeout_us) {
        res.error = "idle timeout waiting for peer";
        break;
      }
    }

    res.wall_us = now_us() - t0;
    res.rcv = receiver.stats();
    res.tx_datagrams = sink.sent();
    res.tx_failed = sink.failed() + sock.send_errors() + sock.truncated_sends();
    res.bytes = total;
    res.sha256 = to_hex(hasher.finish());
    if (res.error.empty()) res.ok = receiver.done();
  } catch (const std::exception& e) {
    res.error = e.what();
  }
  return res;
}

RunResult recv_stream(const Address& bind_addr, std::vector<std::uint8_t>* out,
                      const RunnerConfig& rc, const RecvFilter& filter,
                      std::uint16_t* bound_port) {
  try {
    UdpSocket sock;
    sock.bind_to(bind_addr);
    if (bound_port) *bound_port = sock.local_address().port();
    return recv_stream_on(sock, out, rc, filter);
  } catch (const std::exception& e) {
    RunResult res;
    res.error = e.what();
    return res;
  }
}

}  // namespace rudp::net
