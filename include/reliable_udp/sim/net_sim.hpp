// Deterministic network fault-injection layer.
//
// A SimLink is a DatagramSink, so the protocol engines send into it exactly as
// they would send into a UDP socket. The link owns a time-ordered queue of
// in-flight datagrams and applies loss, duplication, reordering, corruption,
// delay, jitter and rate limiting. Every random decision comes from a single
// seeded std::mt19937_64, and the queue's tie-break is a monotonic insertion
// counter, so a given (config, seed) always produces the identical trace.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include "reliable_udp/datagram_sink.hpp"

namespace rudp::sim {

struct LinkConfig {
  double loss_prob = 0.0;
  double dup_prob = 0.0;
  double reorder_prob = 0.0;
  double corrupt_prob = 0.0;   // flips one random bit in the datagram

  std::uint64_t delay_us = 0;         // base one-way delay
  std::uint64_t jitter_us = 0;        // uniform in [-jitter, +jitter], clamped >= 0
  std::uint64_t reorder_extra_us = 0; // extra delay applied to a reordered packet

  std::uint64_t rate_bps = 0;         // 0 = unlimited
  std::size_t   queue_limit_bytes = 0;  // 0 = unlimited; models a drop-tail buffer

  std::string describe() const;
};

// Scripted (as opposed to probabilistic) fault injection. A test can name the
// exact datagram to damage, which is what makes scenarios like "lose only
// sequence number N" or "duplicate the 3rd ACK" deterministic and readable.
struct FaultDecision {
  bool drop = false;
  bool duplicate = false;
  bool corrupt = false;
  std::uint64_t extra_delay_us = 0;
};

// Return true to take control of this datagram; false to fall through to the
// probabilistic LinkConfig behaviour.
using ScriptFn = std::function<bool(std::uint64_t send_index, const std::uint8_t* buf,
                                    std::size_t len, FaultDecision& out)>;

struct LinkStats {
  std::uint64_t offered = 0;      // datagrams handed to the link
  std::uint64_t offered_bytes = 0;
  std::uint64_t dropped_loss = 0;
  std::uint64_t dropped_queue = 0;
  std::uint64_t dropped_script = 0;
  std::uint64_t duplicated = 0;
  std::uint64_t reordered = 0;    // given extra delay on purpose
  std::uint64_t corrupted = 0;
  std::uint64_t delivered = 0;
  std::uint64_t delivered_bytes = 0;
  std::uint64_t out_of_order_deliveries = 0;  // observed, not injected
};

struct SimDatagram {
  std::uint64_t deliver_us = 0;
  std::uint64_t order = 0;      // insertion counter: deterministic tie-break
  std::uint64_t send_order = 0; // per-link transmit index, to observe reordering
  std::vector<std::uint8_t> bytes;
};

class SimLink final : public DatagramSink {
 public:
  SimLink(std::string name, const LinkConfig& cfg, std::mt19937_64& rng);

  // The driver keeps this pointed at its clock; the link reads it on send.
  void bind_clock(const std::uint64_t* now_us) { now_us_ = now_us; }
  void set_script(ScriptFn fn) { script_ = std::move(fn); }

  void send_datagram(const std::uint8_t* head, std::size_t head_len,
                     const std::uint8_t* payload, std::size_t payload_len) override;

  bool empty() const { return q_.empty(); }
  std::uint64_t next_deliver_us() const {
    return q_.empty() ? UINT64_MAX : q_.top().deliver_us;
  }
  // Moves every datagram whose delivery time has arrived into `out`.
  void drain_due(std::uint64_t now_us, std::vector<SimDatagram>& out);

  const LinkStats& stats() const { return st_; }
  const LinkConfig& config() const { return cfg_; }
  const std::string& name() const { return name_; }

 private:
  struct Cmp {
    bool operator()(const SimDatagram& a, const SimDatagram& b) const {
      if (a.deliver_us != b.deliver_us) return a.deliver_us > b.deliver_us;
      return a.order > b.order;  // FIFO among equal times
    }
  };

  std::uint64_t schedule_time(std::size_t nbytes, std::uint64_t extra_us);
  void enqueue(const std::uint8_t* head, std::size_t head_len, const std::uint8_t* payload,
               std::size_t payload_len, std::uint64_t deliver_us, bool corrupt,
               std::uint64_t send_order);

  std::string name_;
  LinkConfig cfg_;
  std::mt19937_64& rng_;
  LinkStats st_;
  const std::uint64_t* now_us_ = nullptr;
  ScriptFn script_;
  std::vector<std::uint8_t> scratch_;
  std::priority_queue<SimDatagram, std::vector<SimDatagram>, Cmp> q_;
  std::uint64_t order_ = 0;
  std::uint64_t send_order_ = 0;
  std::uint64_t max_delivered_send_order_ = 0;
  std::uint64_t serialise_free_us_ = 0;
  std::size_t queued_bytes_ = 0;
};

}  // namespace rudp::sim
