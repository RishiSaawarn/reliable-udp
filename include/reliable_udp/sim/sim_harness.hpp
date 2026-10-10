// Virtual-clock driver: runs a complete SrSender -> SrReceiver transfer over a
// pair of SimLinks with no real time and no sockets involved.
//
// The loop is a discrete-event simulation. At each step it jumps the clock to
// the earliest of {next datagram delivery, next sender timer, next receiver
// timer}, delivers everything due, then ticks both endpoints. Because nothing
// consults a wall clock, a given (config, seed) is bit-for-bit reproducible and
// a 100 MB transfer over a 50 ms link finishes in milliseconds of CPU time.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "reliable_udp/config.hpp"
#include "reliable_udp/receiver.hpp"
#include "reliable_udp/sender.hpp"
#include "reliable_udp/sim/net_sim.hpp"
#include "reliable_udp/stats.hpp"

namespace rudp::sim {

struct SimParams {
  Config cfg{};
  LinkConfig fwd{};   // sender -> receiver
  LinkConfig rev{};   // receiver -> sender (ACK path)
  std::uint64_t seed = 1;
  std::uint64_t max_virtual_us = 600ull * 1000 * 1000;  // 600 s of virtual time
  std::uint64_t max_steps = 500ull * 1000 * 1000;

  // Optional application backpressure on the receiver: once this many bytes
  // have been delivered, stop reading for `app_pause_us` of virtual time.
  // Used to drive the receive window (and the sender's zero-window probe).
  std::uint64_t app_pause_after_bytes = 0;  // 0 = never pause
  std::uint64_t app_pause_us = 0;

  // Scripted fault injection, applied before the probabilistic model.
  ScriptFn fwd_script;
  ScriptFn rev_script;

  // Ground-truth taps: called with every datagram that actually reaches the
  // peer, in delivery order. Used by the differential tests to reconstruct
  // what the receiver was entitled to deliver.
  std::function<void(const std::uint8_t*, std::size_t, std::uint64_t)> fwd_tap;
  std::function<void(const std::uint8_t*, std::size_t, std::uint64_t)> rev_tap;
};

// Header field accessors for scripts, so a test can say "drop DATA seq N".
PacketType peek_type(const std::uint8_t* buf, std::size_t len);
seq_t peek_seq(const std::uint8_t* buf, std::size_t len);
seq_t peek_ack(const std::uint8_t* buf, std::size_t len);

struct SimReport {
  bool completed = false;
  std::string failure;
  std::uint64_t virtual_us = 0;
  std::uint64_t steps = 0;

  SenderStats snd{};
  ReceiverStats rcv{};
  LinkStats fwd{};
  LinkStats rev{};

  std::uint64_t rtt_samples = 0;
  double srtt_us = 0, rttvar_us = 0, mean_rtt_us = 0;
  std::uint64_t min_rtt_us = 0, max_rtt_us = 0, final_rto_us = 0, base_rto_us = 0;
  std::uint64_t rto_backoffs = 0;

  std::size_t received_bytes = 0;
  bool bytes_match = false;
  std::string sha_sent, sha_received;

  // Derived. Virtual-time goodput: meaningful as a protocol-efficiency figure
  // under the simulated link, NOT as a wall-clock throughput measurement.
  double goodput_mbps() const {
    if (virtual_us == 0) return 0.0;
    return static_cast<double>(received_bytes) * 8.0 / static_cast<double>(virtual_us);
  }
  double retransmit_pct() const {
    if (snd.data_packets_sent == 0) return 0.0;
    return 100.0 * static_cast<double>(snd.retransmits) /
           static_cast<double>(snd.data_packets_sent);
  }
};

SimReport run_sim_transfer(const std::vector<std::uint8_t>& data, const SimParams& params,
                           std::vector<std::uint8_t>* out = nullptr);

// Fills `v` with a deterministic pseudo-random byte pattern.
void fill_pattern(std::vector<std::uint8_t>& v, std::uint64_t seed);

}  // namespace rudp::sim
