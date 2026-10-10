// Independent reference model of the protocol's *observable* state.
//
// Written deliberately differently from the production engines:
//   * the production receiver uses a power-of-two ring buffer indexed by
//     (seq & mask) and compares sequence numbers with wrapping seq_diff();
//     the model uses an ordered std::map keyed by a signed 64-bit packet index
//     measured from the ISN, and compares plain integers.
//   * the production sender tracks per-slot flags in a ring; the model keeps a
//     std::set of outstanding indices.
// The two share no code, so a bug in the ring-buffer indexing or in the
// sequence comparison cannot reproduce itself in the oracle.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "reliable_udp/packet.hpp"
#include "reliable_udp/seq.hpp"

namespace refmodel {

// What a correct receiver must do with a stream of DATA arrivals.
class RefReceiver {
 public:
  RefReceiver(rudp::seq_t isn, std::uint32_t recv_window, std::uint16_t max_payload)
      : isn_(isn), window_(recv_window), max_payload_(max_payload) {}

  enum class Outcome { Accepted, DuplicateBuffered, RecentDuplicate, OutOfWindow, Oversize };

  Outcome on_data(rudp::seq_t seq, const std::uint8_t* payload, std::size_t len);

  // RFC 2883: a duplicate arrival is reported back as a leading D-SACK block on
  // the next ACK. The model predicts it independently of the implementation.
  bool has_dsack() const { return dsack_pending_; }
  rudp::SackBlock take_dsack() {
    dsack_pending_ = false;
    return rudp::SackBlock{dsack_seq_, static_cast<rudp::seq_t>(dsack_seq_ + 1)};
  }
  // The complete SACK block list a correct receiver would put on its next ACK.
  std::vector<rudp::SackBlock> expected_ack_blocks(std::size_t max_blocks);

  const std::vector<std::uint8_t>& delivered() const { return delivered_; }
  std::int64_t base_index() const { return base_; }
  rudp::seq_t rcv_base() const {
    return static_cast<rudp::seq_t>(isn_ + static_cast<rudp::seq_t>(base_));
  }
  std::uint32_t advertised_window() const {
    return window_ - static_cast<std::uint32_t>(buf_.size());
  }
  std::size_t buffered() const { return buf_.size(); }
  // Indices (relative to the ISN) currently held out of order.
  std::set<std::int64_t> buffered_indices() const;
  // The SACK blocks a correct receiver would report, lowest first.
  std::vector<rudp::SackBlock> expected_sacks(std::size_t max_blocks) const;

 private:
  // Signed packet index measured from the ISN. Widening the 32-bit serial
  // difference to int64 makes it exact across the wrap point and, unlike an
  // unsigned index, keeps sequence numbers that precede the ISN negative
  // instead of aliasing them to the far end of the space.
  std::int64_t index_of(rudp::seq_t s) const {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(s - isn_));
  }

  rudp::seq_t isn_;
  std::uint32_t window_;
  std::uint16_t max_payload_;
  std::int64_t base_ = 0;
  std::map<std::int64_t, std::vector<std::uint8_t>> buf_;
  std::vector<std::uint8_t> delivered_;
  bool dsack_pending_ = false;
  rudp::seq_t dsack_seq_ = 0;
};

// What a correct sender must do with a stream of ACK/SACK events. This models
// the *permitted* state, not the retransmission policy, so it stays independent
// of how the real sender decides to retransmit.
class RefSender {
 public:
  RefSender(rudp::seq_t isn, std::int64_t total_packets, std::uint32_t window)
      : isn_(isn), total_(total_packets), window_(window) {}

  // Records that the implementation transmitted this sequence number.
  // Returns an error string if that transmission violated a protocol rule.
  const char* note_transmit(rudp::seq_t seq);
  void note_ack(rudp::seq_t cum_ack, const rudp::SackBlock* sacks, std::size_t n);

  std::int64_t base_index() const { return base_; }
  rudp::seq_t snd_base() const {
    return static_cast<rudp::seq_t>(isn_ + static_cast<rudp::seq_t>(base_));
  }
  std::int64_t highest_sent_index() const { return high_; }
  std::set<std::int64_t> outstanding() const;
  bool all_sent() const { return high_ >= total_; }
  std::uint64_t transmissions(std::int64_t index) const {
    auto it = tx_count_.find(index);
    return it == tx_count_.end() ? 0 : it->second;
  }

 private:
  std::int64_t index_of(rudp::seq_t s) const {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(s - isn_));
  }
  rudp::seq_t isn_;
  std::int64_t total_;
  std::uint32_t window_;
  std::int64_t base_ = 0;   // lowest index not cumulatively acknowledged
  std::int64_t high_ = 0;   // 1 + highest index ever transmitted
  std::set<std::int64_t> sacked_;
  std::map<std::int64_t, std::uint64_t> tx_count_;
};

}  // namespace refmodel
