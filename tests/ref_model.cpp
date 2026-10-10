#include "ref_model.hpp"

#include <algorithm>

namespace refmodel {

using rudp::SackBlock;
using rudp::seq_t;

RefReceiver::Outcome RefReceiver::on_data(seq_t seq, const std::uint8_t* payload,
                                          std::size_t len) {
  if (len > max_payload_) return Outcome::Oversize;
  const std::int64_t idx = index_of(seq);
  if (idx < base_) {
    // Only a duplicate from within the last window is recent enough to report.
    if (base_ - idx <= static_cast<std::int64_t>(window_)) {
      dsack_pending_ = true;
      dsack_seq_ = seq;
      return Outcome::RecentDuplicate;
    }
    return Outcome::OutOfWindow;
  }
  if (idx >= base_ + static_cast<std::int64_t>(window_)) return Outcome::OutOfWindow;
  if (buf_.find(idx) != buf_.end()) {
    dsack_pending_ = true;
    dsack_seq_ = seq;
    return Outcome::DuplicateBuffered;
  }

  buf_.emplace(idx, std::vector<std::uint8_t>(payload, payload + len));
  // Deliver every contiguous packet starting at the base.
  for (;;) {
    auto it = buf_.find(base_);
    if (it == buf_.end()) break;
    delivered_.insert(delivered_.end(), it->second.begin(), it->second.end());
    buf_.erase(it);
    ++base_;
  }
  return Outcome::Accepted;
}

std::vector<SackBlock> RefReceiver::expected_ack_blocks(std::size_t max_blocks) {
  std::vector<SackBlock> out;
  if (dsack_pending_) out.push_back(take_dsack());
  const auto rest = expected_sacks(max_blocks - out.size());
  out.insert(out.end(), rest.begin(), rest.end());
  return out;
}

std::set<std::int64_t> RefReceiver::buffered_indices() const {
  std::set<std::int64_t> s;
  for (const auto& [k, v] : buf_) s.insert(k);
  return s;
}

std::vector<SackBlock> RefReceiver::expected_sacks(std::size_t max_blocks) const {
  std::vector<SackBlock> out;
  auto it = buf_.begin();
  while (it != buf_.end() && out.size() < max_blocks) {
    const std::int64_t start = it->first;
    std::int64_t end = start + 1;
    ++it;
    while (it != buf_.end() && it->first == end) {
      ++end;
      ++it;
    }
    out.push_back(SackBlock{static_cast<seq_t>(isn_ + static_cast<seq_t>(start)),
                            static_cast<seq_t>(isn_ + static_cast<seq_t>(end))});
  }
  return out;
}

const char* RefSender::note_transmit(seq_t seq) {
  const std::int64_t idx = index_of(seq);
  if (idx < 0) return "transmitted a sequence number before the start of the stream";
  if (idx >= total_) return "transmitted a sequence number past the end of the stream";
  if (idx < base_) return "retransmitted a packet that was already cumulatively acknowledged";
  if (idx >= base_ + static_cast<std::int64_t>(window_))
    return "transmitted beyond the sender window";
  if (idx > high_) {
    if (idx != high_) return "skipped a sequence number";
  }
  if (sacked_.count(idx) != 0) return "retransmitted a packet the peer already SACKed";
  ++tx_count_[idx];
  high_ = std::max(high_, idx + 1);
  return nullptr;
}

void RefSender::note_ack(seq_t cum_ack, const SackBlock* sacks, std::size_t n) {
  const std::int64_t idx = index_of(cum_ack);
  if (idx > base_ && idx <= high_) {
    base_ = idx;
    // Anything below the new base is no longer interesting.
    while (!sacked_.empty() && *sacked_.begin() < base_) sacked_.erase(sacked_.begin());
  }
  for (std::size_t i = 0; i < n; ++i) {
    const std::int64_t s = index_of(sacks[i].start);
    const std::int64_t e = index_of(sacks[i].end);
    for (std::int64_t k = s; k < e && k < high_; ++k) {
      if (k >= base_) sacked_.insert(k);
    }
  }
}

std::set<std::int64_t> RefSender::outstanding() const {
  std::set<std::int64_t> s;
  for (std::int64_t i = base_; i < high_; ++i) {
    if (sacked_.count(i) == 0) s.insert(i);
  }
  return s;
}

}  // namespace refmodel
