#include "reliable_udp/sim/sim_harness.hpp"

#include <algorithm>
#include <random>

#include "reliable_udp/packet.hpp"
#include "reliable_udp/sha256.hpp"

namespace rudp::sim {

PacketType peek_type(const std::uint8_t* buf, std::size_t len) {
  if (len < kHeaderSize) return PacketType::Rst;
  return static_cast<PacketType>(buf[1]);
}
static seq_t be32(const std::uint8_t* p) {
  return (static_cast<seq_t>(p[0]) << 24) | (static_cast<seq_t>(p[1]) << 16) |
         (static_cast<seq_t>(p[2]) << 8) | static_cast<seq_t>(p[3]);
}
seq_t peek_seq(const std::uint8_t* buf, std::size_t len) {
  return len < kHeaderSize ? 0 : be32(buf + 8);
}
seq_t peek_ack(const std::uint8_t* buf, std::size_t len) {
  return len < kHeaderSize ? 0 : be32(buf + 12);
}

void fill_pattern(std::vector<std::uint8_t>& v, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::size_t i = 0;
  while (i + 8 <= v.size()) {
    const std::uint64_t x = rng();
    for (int k = 0; k < 8; ++k) v[i + static_cast<std::size_t>(k)] =
        static_cast<std::uint8_t>(x >> (8 * k));
    i += 8;
  }
  const std::uint64_t x = rng();
  for (int k = 0; i < v.size(); ++i, ++k) v[i] = static_cast<std::uint8_t>(x >> (8 * k));
}

SimReport run_sim_transfer(const std::vector<std::uint8_t>& data, const SimParams& params,
                           std::vector<std::uint8_t>* out) {
  SimReport rep;
  std::vector<std::uint8_t> local;
  std::vector<std::uint8_t>& sink_buf = out ? *out : local;
  sink_buf.clear();
  sink_buf.reserve(data.size());

  std::uint64_t now = 0;
  std::mt19937_64 rng(params.seed);
  SimLink fwd("fwd", params.fwd, rng);
  SimLink rev("rev", params.rev, rng);
  fwd.bind_clock(&now);
  rev.bind_clock(&now);
  if (params.fwd_script) fwd.set_script(params.fwd_script);
  if (params.rev_script) rev.set_script(params.rev_script);

  SrSender sender(params.cfg, fwd, data.empty() ? nullptr : data.data(), data.size());
  bool paused = false;
  std::uint64_t resume_at = UINT64_MAX;

  SrReceiver receiver(params.cfg, rev, [&](const std::uint8_t* p, std::size_t n) {
    sink_buf.insert(sink_buf.end(), p, p + n);
  });

  sender.start(now);

  std::vector<SimDatagram> due;
  due.reserve(64);

  while (true) {
    if (sender.done() && receiver.done()) {
      rep.completed = true;
      break;
    }
    if (sender.failed()) {
      rep.failure = std::string("sender failed: ") + sender.failure_reason();
      break;
    }
    if (rep.steps++ > params.max_steps) {
      rep.failure = "step limit exceeded";
      break;
    }

    std::uint64_t next = std::min(fwd.next_deliver_us(), rev.next_deliver_us());
    next = std::min(next, sender.next_deadline_us());
    next = std::min(next, receiver.next_deadline_us());
    if (paused) next = std::min(next, resume_at);

    if (next == UINT64_MAX) {
      rep.failure = "deadlock: no pending events, no pending timers, transfer incomplete";
      break;
    }
    if (next > params.max_virtual_us) {
      rep.failure = "virtual time limit exceeded";
      break;
    }
    if (next > now) now = next;

    due.clear();
    fwd.drain_due(now, due);
    for (const SimDatagram& d : due) {
      if (params.fwd_tap) params.fwd_tap(d.bytes.data(), d.bytes.size(), now);
      receiver.on_datagram(d.bytes.data(), d.bytes.size(), now);
    }

    due.clear();
    rev.drain_due(now, due);
    for (const SimDatagram& d : due) {
      if (params.rev_tap) params.rev_tap(d.bytes.data(), d.bytes.size(), now);
      sender.on_datagram(d.bytes.data(), d.bytes.size(), now);
    }

    if (params.app_pause_after_bytes > 0) {
      if (!paused && !receiver.app_paused() && resume_at == UINT64_MAX &&
          receiver.stats().data_bytes_delivered >= params.app_pause_after_bytes) {
        paused = true;
        resume_at = now + params.app_pause_us;
        receiver.set_app_paused(true, now);
      } else if (paused && now >= resume_at) {
        paused = false;
        receiver.set_app_paused(false, now);
        resume_at = 0;  // never pause again
      }
    }

    sender.tick(now);
    receiver.tick(now);
  }

  rep.virtual_us = now;
  rep.snd = sender.stats();
  rep.rcv = receiver.stats();
  rep.fwd = fwd.stats();
  rep.rev = rev.stats();
  rep.rtt_samples = sender.rtt().samples();
  rep.srtt_us = sender.rtt().srtt_us();
  rep.rttvar_us = sender.rtt().rttvar_us();
  rep.mean_rtt_us = sender.rtt().mean_rtt_us();
  rep.min_rtt_us = sender.rtt().min_rtt_us();
  rep.max_rtt_us = sender.rtt().max_rtt_us();
  rep.final_rto_us = sender.rtt().rto_us();
  rep.base_rto_us = sender.rtt().base_rto_us();
  rep.rto_backoffs = sender.rtt().timeouts();
  rep.received_bytes = sink_buf.size();
  rep.bytes_match = (sink_buf.size() == data.size()) &&
                    (data.empty() || std::equal(data.begin(), data.end(), sink_buf.begin()));
  rep.sha_sent = sha256_hex(data.empty() ? nullptr : data.data(), data.size());
  rep.sha_received = sha256_hex(sink_buf.empty() ? nullptr : sink_buf.data(), sink_buf.size());
  return rep;
}

}  // namespace rudp::sim
