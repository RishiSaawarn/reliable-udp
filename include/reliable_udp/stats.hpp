#pragma once
#include <cstdint>

namespace rudp {

struct SenderStats {
  std::uint64_t data_packets_sent      = 0;  // includes retransmissions
  std::uint64_t data_bytes_sent        = 0;  // payload bytes on the wire
  std::uint64_t app_bytes              = 0;  // distinct payload bytes
  std::uint64_t retransmits            = 0;
  std::uint64_t timeout_retransmits    = 0;
  std::uint64_t fast_retransmits       = 0;  // SACK-triggered
  std::uint64_t zero_window_probes     = 0;
  std::uint64_t control_packets_sent   = 0;  // SYN / FIN and their retries
  std::uint64_t acks_received          = 0;
  std::uint64_t dup_acks               = 0;
  std::uint64_t sack_blocks_received   = 0;
  std::uint64_t dsacks_received        = 0;
  std::uint64_t reorder_window_us      = 0;  // final RACK reordering window
  std::uint64_t window_stalls          = 0;  // send blocked by cwnd/rwnd
  std::uint64_t rx_bad_checksum        = 0;
  std::uint64_t rx_bad_version         = 0;
  std::uint64_t rx_bad_session         = 0;
  std::uint64_t rx_malformed           = 0;
  std::uint64_t rx_ignored             = 0;  // well-formed but not applicable
};

struct ReceiverStats {
  std::uint64_t data_packets_received  = 0;
  std::uint64_t data_bytes_delivered   = 0;
  std::uint64_t dup_data               = 0;  // already buffered or delivered
  std::uint64_t out_of_order           = 0;  // accepted above rcv_base
  std::uint64_t out_of_window          = 0;  // above rcv_base + rwnd
  std::uint64_t acks_sent              = 0;
  std::uint64_t sack_blocks_sent       = 0;
  std::uint64_t dsacks_sent            = 0;
  std::uint64_t max_buffered           = 0;  // high-water out-of-order count
  std::uint64_t zero_window_advertised = 0;
  std::uint64_t rx_bad_checksum        = 0;
  std::uint64_t rx_bad_version         = 0;
  std::uint64_t rx_bad_session         = 0;
  std::uint64_t rx_malformed           = 0;
  std::uint64_t rx_ignored             = 0;
};

}  // namespace rudp
