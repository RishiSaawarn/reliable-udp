// Single-session, single-threaded event loop over a real UDP socket.
//
// Same engines as the simulator; only the clock and the sink change. The loop
// is: compute the engine's next deadline, ppoll() until then or until the
// socket is readable, drain every queued datagram, tick, repeat.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "reliable_udp/config.hpp"
#include "reliable_udp/net/udp_socket.hpp"
#include "reliable_udp/rtt.hpp"
#include "reliable_udp/stats.hpp"

namespace rudp::net {

// Test-only hook: return false to pretend the datagram never arrived.
// Production callers pass nullptr. Used to inject deterministic loss on a real
// socket path without needing an actual lossy network.
using RecvFilter = std::function<bool(const std::uint8_t*, std::size_t)>;

struct RunnerConfig {
  Config proto{};
  int rcvbuf_bytes = 8 << 20;
  int sndbuf_bytes = 8 << 20;
  std::uint64_t poll_slice_us = 2000;      // upper bound on a single ppoll wait
  std::uint64_t idle_timeout_us = 30ull * 1000 * 1000;
  std::uint32_t max_burst_reads = 64;      // datagrams drained per wakeup
};

struct RttSnapshot {
  std::uint64_t samples = 0;
  double srtt_us = 0, rttvar_us = 0, mean_us = 0;
  std::uint64_t min_us = 0, max_us = 0, final_rto_us = 0, base_rto_us = 0, backoffs = 0;
};

struct RunResult {
  bool ok = false;
  std::string error;
  std::uint64_t wall_us = 0;
  SenderStats snd{};
  ReceiverStats rcv{};
  RttSnapshot rtt{};
  std::uint64_t rx_datagrams = 0;
  std::uint64_t rx_bytes = 0;
  std::uint64_t tx_datagrams = 0;
  std::uint64_t tx_failed = 0;
  std::uint64_t filtered_out = 0;
  std::uint64_t poll_wakeups = 0;
  std::uint64_t socket_rcvbuf = 0;
  std::size_t bytes = 0;
  std::string sha256;
};

// Sends `data` to `peer`. Returns when the session closes or fails.
RunResult send_stream(const Address& peer, const std::uint8_t* data, std::size_t len,
                      const RunnerConfig& rc, const RecvFilter& filter = nullptr);

// Binds `bind_addr`, accepts one session, appends the stream to `out`
// (may be null to only hash), returns when the session closes.
RunResult recv_stream(const Address& bind_addr, std::vector<std::uint8_t>* out,
                      const RunnerConfig& rc, const RecvFilter& filter = nullptr,
                      std::uint16_t* bound_port = nullptr);

// Same as recv_stream but on an already-bound socket, so a test can learn the
// ephemeral port before the sender starts.
RunResult recv_stream_on(UdpSocket& sock, std::vector<std::uint8_t>* out,
                         const RunnerConfig& rc, const RecvFilter& filter = nullptr);

}  // namespace rudp::net
