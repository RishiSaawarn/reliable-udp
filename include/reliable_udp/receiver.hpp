#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "reliable_udp/config.hpp"
#include "reliable_udp/datagram_sink.hpp"
#include "reliable_udp/packet.hpp"
#include "reliable_udp/seq.hpp"
#include "reliable_udp/stats.hpp"

namespace rudp {

// Selective Repeat receiver. Mirrors SrSender: pure logic, explicit clock.
class SrReceiver {
 public:
  // Called with each in-order run of bytes, exactly once per byte, in order.
  using DeliverFn = std::function<void(const std::uint8_t*, std::size_t)>;

  SrReceiver(const Config& cfg, DatagramSink& sink, DeliverFn deliver);

  void on_datagram(const std::uint8_t* buf, std::size_t len, std::uint64_t now_us);
  void tick(std::uint64_t now_us);
  std::uint64_t next_deadline_us() const;

  enum class State { Listen, Established, Closing, Closed };
  State state() const { return state_; }
  bool done() const { return state_ == State::Closed; }
  static const char* state_name(State s);

  // Application backpressure. While paused, in-order data stays in the
  // reassembly buffer instead of being delivered, so the advertised window
  // shrinks to zero and the sender must fall back to zero-window probing.
  // This is how receiver-window exhaustion is exercised deterministically.
  void set_app_paused(bool paused, std::uint64_t now_us);
  bool app_paused() const { return app_paused_; }

  const ReceiverStats& stats() const { return st_; }
  seq_t rcv_base() const { return rcv_base_; }
  std::uint32_t buffered() const { return buffered_; }
  std::uint32_t advertised_window() const;

 private:
  struct Slot {
    std::vector<std::uint8_t> buf;
    std::uint16_t len = 0;
    bool present = false;
  };

  Slot& slot(seq_t s) { return slots_[s & mask_]; }
  void deliver_contiguous();
  void send_ack(PacketType type, std::uint64_t now_us);
  std::size_t build_sacks(SackBlock* out, std::size_t max_blocks) const;
  void note_duplicate(seq_t seq);

  Config cfg_;
  DatagramSink& sink_;
  DeliverFn deliver_;

  ReceiverStats st_;
  State state_ = State::Listen;

  std::uint32_t mask_ = 0;
  std::vector<Slot> slots_;
  std::uint32_t buffered_ = 0;
  bool app_paused_ = false;

  std::uint32_t session_id_ = 0;
  seq_t isn_ = 0;
  seq_t rcv_base_ = 0;

  // RFC 2883 D-SACK: the sequence number of the most recent duplicate arrival,
  // reported in the first SACK block of the next ACK. It is the only way the
  // sender can learn that the network is reordering rather than dropping.
  bool dsack_pending_ = false;
  seq_t dsack_seq_ = 0;

  bool fin_seen_ = false;
  seq_t fin_seq_ = 0;

  std::uint32_t unacked_since_ = 0;
  std::uint64_t ack_deadline_us_ = UINT64_MAX;
  std::uint64_t close_deadline_us_ = UINT64_MAX;

  std::uint8_t head_buf_[kMaxOverhead];
};

}  // namespace rudp
