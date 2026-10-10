#pragma once
#include <cstddef>
#include <cstdint>

#include "reliable_udp/seq.hpp"

namespace rudp {

// Rounds v up to a power of two (v >= 1).
inline std::uint32_t round_up_pow2(std::uint32_t v) noexcept {
  if (v <= 1) return 1;
  --v;
  v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
  return v + 1;
}

// All durations are microseconds. All windows are counted in packets.
struct Config {
  std::uint16_t max_payload = 1200;

  // Sliding-window sizes. normalize() rounds both up to a power of two so the
  // ring buffers can index with a mask; the rounded values are what the
  // protocol actually uses and what statistics report.
  std::uint32_t send_window = 64;
  std::uint32_t recv_window = 128;

  // RFC 6298 retransmission timer bounds. Note that min_rto deliberately
  // deviates from RFC 6298's 1 s floor; see docs/PROTOCOL.md.
  std::uint64_t init_rto_us = 200000;   // used before the first RTT sample
  std::uint64_t min_rto_us  = 20000;
  std::uint64_t max_rto_us  = 2000000;
  std::uint64_t clock_granularity_us = 1000;  // G in RFC 6298

  std::uint32_t max_retx_per_packet = 20;   // give up on the session past this
  std::uint32_t handshake_retries   = 10;
  std::uint32_t dup_sack_threshold  = 3;    // RFC 6675 "DupThresh"

  // Reordering tolerance for SACK-based loss inference, in the spirit of
  // RFC 8985 (RACK): a hole is only declared lost once it is also older than
  // SRTT / reorder_window_divisor. Without it, any reordering deeper than
  // dup_sack_threshold packets is misread as loss and retransmitted for
  // nothing. Set to 0 to disable the delay and get pure RFC 6675 behaviour
  // (the benchmarks use this to measure what the window is worth). Following
  // RFC 8985 the window starts at SRTT/divisor and grows by that step on every
  // further duplicate report, capped at one SRTT.
  std::uint32_t reorder_window_divisor = 4;
  std::uint32_t reorder_window_max_mult = 16;

  // Delayed-ACK. ack_every == 1 means every DATA packet is acknowledged
  // immediately and ack_delay_us is never armed.
  std::uint32_t ack_every    = 1;
  std::uint64_t ack_delay_us = 2000;

  std::uint64_t linger_us            = 200000;  // receiver post-FIN quiet time
  std::uint64_t zero_window_probe_us = 100000;

  seq_t         isn        = 1;
  std::uint32_t session_id = 0x52554450u;  // "RUDP"

  Config& normalize() {
    if (max_payload == 0) max_payload = 1;
    send_window = round_up_pow2(send_window);
    recv_window = round_up_pow2(recv_window);
    if (ack_every == 0) ack_every = 1;
    if (min_rto_us > max_rto_us) min_rto_us = max_rto_us;
    return *this;
  }
};

}  // namespace rudp
