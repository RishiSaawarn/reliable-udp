#include "reliable_udp/sim/net_sim.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rudp::sim {

std::string LinkConfig::describe() const {
  char b[384];
  std::snprintf(b, sizeof(b),
                "loss=%.4g dup=%.4g reorder=%.4g corrupt=%.4g delay=%lluus jitter=%lluus "
                "reorder_extra=%lluus rate=%llubps qlimit=%zuB",
                loss_prob, dup_prob, reorder_prob, corrupt_prob,
                static_cast<unsigned long long>(delay_us),
                static_cast<unsigned long long>(jitter_us),
                static_cast<unsigned long long>(reorder_extra_us),
                static_cast<unsigned long long>(rate_bps), queue_limit_bytes);
  return std::string(b);
}

SimLink::SimLink(std::string name, const LinkConfig& cfg, std::mt19937_64& rng)
    : name_(std::move(name)), cfg_(cfg), rng_(rng) {}

std::uint64_t SimLink::schedule_time(std::size_t nbytes, std::uint64_t extra_us) {
  const std::uint64_t now = *now_us_;
  std::uint64_t start = now;
  if (cfg_.rate_bps > 0) {
    // Serialise onto the link: a packet cannot start before the previous one
    // has finished clocking out.
    if (serialise_free_us_ > start) start = serialise_free_us_;
    const std::uint64_t serialise_us =
        (static_cast<std::uint64_t>(nbytes) * 8ull * 1000000ull) / cfg_.rate_bps;
    serialise_free_us_ = start + serialise_us;
    start = serialise_free_us_;
  }
  std::uint64_t t = start + cfg_.delay_us;
  if (cfg_.jitter_us > 0) {
    std::uniform_int_distribution<std::int64_t> d(-static_cast<std::int64_t>(cfg_.jitter_us),
                                                  static_cast<std::int64_t>(cfg_.jitter_us));
    const std::int64_t j = d(rng_);
    if (j < 0 && static_cast<std::uint64_t>(-j) > t - now) {
      t = now;  // never travel back before the send instant
    } else {
      t = static_cast<std::uint64_t>(static_cast<std::int64_t>(t) + j);
    }
  }
  return t + extra_us;
}

void SimLink::enqueue(const std::uint8_t* head, std::size_t head_len,
                      const std::uint8_t* payload, std::size_t payload_len,
                      std::uint64_t deliver_us, bool corrupt, std::uint64_t send_order) {
  SimDatagram d;
  d.deliver_us = deliver_us;
  d.order = order_++;
  d.send_order = send_order;
  d.bytes.resize(head_len + payload_len);
  std::memcpy(d.bytes.data(), head, head_len);
  if (payload_len > 0) std::memcpy(d.bytes.data() + head_len, payload, payload_len);
  if (corrupt && !d.bytes.empty()) {
    std::uniform_int_distribution<std::size_t> bd(0, d.bytes.size() - 1);
    std::uniform_int_distribution<int> bitd(0, 7);
    const std::size_t idx = bd(rng_);
    const int bit = bitd(rng_);
    d.bytes[idx] = static_cast<std::uint8_t>(d.bytes[idx] ^ (1u << bit));
    ++st_.corrupted;
  }
  queued_bytes_ += d.bytes.size();
  q_.push(std::move(d));
}

void SimLink::send_datagram(const std::uint8_t* head, std::size_t head_len,
                            const std::uint8_t* payload, std::size_t payload_len) {
  const std::size_t total = head_len + payload_len;
  ++st_.offered;
  st_.offered_bytes += total;
  const std::uint64_t my_send_order = send_order_++;

  if (cfg_.queue_limit_bytes > 0 && queued_bytes_ + total > cfg_.queue_limit_bytes) {
    ++st_.dropped_queue;
    return;
  }

  bool scripted = false;
  FaultDecision fd;
  if (script_) {
    // Give the script a contiguous view; the engines hand us two fragments.
    scratch_.resize(total);
    std::memcpy(scratch_.data(), head, head_len);
    if (payload_len > 0) std::memcpy(scratch_.data() + head_len, payload, payload_len);
    scripted = script_(my_send_order, scratch_.data(), total, fd);
  }

  bool drop, dup, corrupt;
  std::uint64_t extra = 0;
  if (scripted) {
    drop = fd.drop;
    dup = fd.duplicate;
    corrupt = fd.corrupt;
    extra = fd.extra_delay_us;
    if (drop) ++st_.dropped_script;
    if (extra > 0) ++st_.reordered;
  } else {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    drop = cfg_.loss_prob > 0.0 && u(rng_) < cfg_.loss_prob;
    if (drop) ++st_.dropped_loss;
    const bool reorder = !drop && cfg_.reorder_prob > 0.0 && u(rng_) < cfg_.reorder_prob;
    if (reorder) {
      ++st_.reordered;
      extra = cfg_.reorder_extra_us;
    }
    corrupt = !drop && cfg_.corrupt_prob > 0.0 && u(rng_) < cfg_.corrupt_prob;
    dup = !drop && cfg_.dup_prob > 0.0 && u(rng_) < cfg_.dup_prob;
  }
  if (drop) return;

  enqueue(head, head_len, payload, payload_len, schedule_time(total, extra), corrupt,
          my_send_order);
  if (dup) {
    ++st_.duplicated;
    // The copy is scheduled independently, so it may arrive before or after
    // the original.
    enqueue(head, head_len, payload, payload_len, schedule_time(total, 0), false,
            my_send_order);
  }
}

void SimLink::drain_due(std::uint64_t now_us, std::vector<SimDatagram>& out) {
  while (!q_.empty() && q_.top().deliver_us <= now_us) {
    SimDatagram d = q_.top();
    q_.pop();
    queued_bytes_ -= d.bytes.size();
    ++st_.delivered;
    st_.delivered_bytes += d.bytes.size();
    // "Out of order" means: something transmitted later on this link was
    // already delivered before this one.
    if (d.send_order < max_delivered_send_order_) ++st_.out_of_order_deliveries;
    max_delivered_send_order_ = std::max(max_delivered_send_order_, d.send_order);
    out.push_back(std::move(d));
  }
}

}  // namespace rudp::sim
