#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "reliable_udp/config.hpp"
#include "reliable_udp/datagram_sink.hpp"
#include "reliable_udp/packet.hpp"
#include "reliable_udp/rtt.hpp"
#include "reliable_udp/seq.hpp"
#include "reliable_udp/stats.hpp"

namespace rudp {

// Selective Repeat sender for one unidirectional bulk transfer.
//
// Pure logic: it never touches a socket or a clock. The driver feeds it
// datagrams and a monotonic microsecond timestamp, and it pushes datagrams
// back out through the DatagramSink. That is what makes the simulator able to
// run the exact same code under a virtual clock.
class SrSender {
 public:
  enum class State { Idle, SynSent, Established, FinSent, Closed, Failed };

  SrSender(const Config& cfg, DatagramSink& sink, const std::uint8_t* data, std::size_t len);

  void start(std::uint64_t now_us);
  void on_datagram(const std::uint8_t* buf, std::size_t len, std::uint64_t now_us);
  void tick(std::uint64_t now_us);

  // Absolute time of the next thing this endpoint wants to do, or UINT64_MAX.
  std::uint64_t next_deadline_us() const;

  State state() const { return state_; }
  bool done() const { return state_ == State::Closed; }
  bool failed() const { return state_ == State::Failed; }
  const char* failure_reason() const { return failure_reason_; }
  static const char* state_name(State s);

  const SenderStats& stats() const { return st_; }
  const RttEstimator& rtt() const { return rtt_; }
  const Config& config() const { return cfg_; }

  seq_t snd_base() const { return snd_base_; }
  seq_t snd_next() const { return snd_next_; }
  std::uint32_t peer_window() const { return rwnd_; }
  std::uint32_t in_flight() const {
    return static_cast<std::uint32_t>(seq_diff(snd_next_, snd_base_));
  }

 private:
  struct Slot {
    std::size_t   offset = 0;
    std::uint64_t last_send_us = 0;
    std::uint64_t rto_deadline_us = 0;
    std::uint16_t len = 0;
    std::uint16_t retx = 0;
    bool in_use = false;
    bool acked = false;
    bool rtt_eligible = false;   // Karn: cleared on any retransmission
    bool fast_rexmitted = false;
  };

  Slot& slot(seq_t s) { return slots_[s & mask_]; }
  const Slot& slot(seq_t s) const { return slots_[s & mask_]; }

  void send_control(PacketType type, seq_t seq, std::uint64_t now_us);
  void transmit_data(seq_t s, std::uint64_t now_us);
  void fill_window(std::uint64_t now_us);
  void handle_ack(const Header& h, const SackBlock* sacks, std::size_t n, std::uint64_t now_us);
  void detect_sack_losses(std::uint64_t now_us);
  void check_transfer_complete(std::uint64_t now_us);
  void arm_persist_if_stuck(std::uint64_t now_us);
  void send_persist_probe(std::uint64_t now_us);
  void fail(const char* why);
  std::uint32_t effective_window() const;

  Config cfg_;
  DatagramSink& sink_;
  const std::uint8_t* data_;
  std::size_t len_;

  RttEstimator rtt_;
  SenderStats st_;
  State state_ = State::Idle;
  const char* failure_reason_ = "";

  std::uint32_t mask_ = 0;
  std::vector<Slot> slots_;

  seq_t isn_ = 0;
  seq_t snd_base_ = 0;   // oldest unacknowledged
  seq_t snd_next_ = 0;   // next never-sent
  seq_t final_seq_ = 0;  // one past the last data sequence number
  std::size_t queued_offset_ = 0;
  std::uint32_t rwnd_ = 1;  // peer's advertised window, in packets

  // RACK state (RFC 8985, simplified). rack_xmit_us_ is the transmission time
  // of the most recently transmitted packet that has been acknowledged; a hole
  // older than that by more than the reordering window is inferred lost.
  // The reordering window stays at zero -- i.e. pure RFC 6675 dup-threshold
  // behaviour -- until a D-SACK proves the path actually reorders.
  std::uint64_t rack_xmit_us_ = 0;
  bool reordering_seen_ = false;
  std::uint32_t reorder_window_mult_ = 1;

  std::uint64_t control_deadline_us_ = UINT64_MAX;
  std::uint32_t control_retries_ = 0;
  // Persist timer. Armed whenever the sender is Established, has not finished,
  // and has nothing that would otherwise wake it up: no unacknowledged packet
  // with a running RTO and no room in the window. Without it a lost window
  // update would deadlock the connection forever.
  std::uint64_t persist_deadline_us_ = UINT64_MAX;
  std::uint64_t persist_interval_us_ = 0;
  std::uint64_t syn_send_us_ = 0;
  bool syn_rtt_eligible_ = true;

  std::uint8_t head_buf_[kMaxOverhead];
};

}  // namespace rudp
