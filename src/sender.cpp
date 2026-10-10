#include "reliable_udp/sender.hpp"

#include <algorithm>

namespace rudp {
namespace {

// RFC 2883: the first SACK block is a D-SACK -- a report of a duplicate arrival
// rather than new information -- when it lies entirely at or below the
// cumulative ACK, or when it is a subset of one of the following blocks.
bool is_dsack(const SackBlock* sacks, std::size_t n, seq_t cum_ack) {
  if (n == 0) return false;
  const SackBlock& first = sacks[0];
  if (seq_le(first.end, cum_ack)) return true;
  for (std::size_t j = 1; j < n; ++j) {
    if (seq_ge(first.start, sacks[j].start) && seq_le(first.end, sacks[j].end)) return true;
  }
  return false;
}

}  // namespace

SrSender::SrSender(const Config& cfg, DatagramSink& sink, const std::uint8_t* data,
                   std::size_t len)
    : cfg_(Config(cfg).normalize()),
      sink_(sink),
      data_(data),
      len_(len),
      rtt_(cfg_),
      slots_(cfg_.send_window) {
  mask_ = cfg_.send_window - 1;
  isn_ = cfg_.isn;
  snd_base_ = isn_;
  snd_next_ = isn_;
  const std::size_t pkts =
      (len_ + cfg_.max_payload - 1) / cfg_.max_payload;  // 0 for an empty transfer
  final_seq_ = static_cast<seq_t>(isn_ + static_cast<seq_t>(pkts));
  st_.app_bytes = len_;
  persist_interval_us_ = cfg_.zero_window_probe_us;
}

const char* SrSender::state_name(State s) {
  switch (s) {
    case State::Idle:        return "IDLE";
    case State::SynSent:     return "SYN_SENT";
    case State::Established: return "ESTABLISHED";
    case State::FinSent:     return "FIN_SENT";
    case State::Closed:      return "CLOSED";
    case State::Failed:      return "FAILED";
  }
  return "?";
}

void SrSender::fail(const char* why) {
  if (state_ != State::Failed) {
    state_ = State::Failed;
    failure_reason_ = why;
  }
}

std::uint32_t SrSender::effective_window() const {
  return std::min<std::uint32_t>(cfg_.send_window, rwnd_);
}

void SrSender::send_control(PacketType type, seq_t s, std::uint64_t now_us) {
  Header h;
  h.type = type;
  h.session_id = cfg_.session_id;
  h.seq = s;
  h.ack = 0;
  h.window = 0;
  const std::size_t n = encode_packet(head_buf_, h, nullptr, 0, nullptr, 0);
  sink_.send_datagram(head_buf_, n, nullptr, 0);
  ++st_.control_packets_sent;
  control_deadline_us_ = now_us + rtt_.rto_us();
}

void SrSender::start(std::uint64_t now_us) {
  if (state_ != State::Idle) return;
  state_ = State::SynSent;
  control_retries_ = 0;
  syn_send_us_ = now_us;
  syn_rtt_eligible_ = true;
  send_control(PacketType::Syn, isn_, now_us);
}

void SrSender::transmit_data(seq_t s, std::uint64_t now_us) {
  Slot& sl = slot(s);
  Header h;
  h.type = PacketType::Data;
  h.session_id = cfg_.session_id;
  h.seq = s;
  h.ack = 0;
  h.window = 0;
  const std::size_t n =
      encode_packet(head_buf_, h, nullptr, 0, data_ + sl.offset, sl.len);
  sink_.send_datagram(head_buf_, n, data_ + sl.offset, sl.len);
  sl.last_send_us = now_us;
  sl.rto_deadline_us = now_us + rtt_.rto_us();
  ++st_.data_packets_sent;
  st_.data_bytes_sent += sl.len;
}

void SrSender::fill_window(std::uint64_t now_us) {
  const std::uint32_t win = effective_window();
  bool stalled = false;
  while (queued_offset_ < len_) {
    if (static_cast<std::uint32_t>(seq_diff(snd_next_, snd_base_)) >= win) {
      stalled = true;
      break;
    }
    Slot& sl = slot(snd_next_);
    const std::size_t remaining = len_ - queued_offset_;
    sl.offset = queued_offset_;
    sl.len = static_cast<std::uint16_t>(std::min<std::size_t>(remaining, cfg_.max_payload));
    sl.retx = 0;
    sl.in_use = true;
    sl.acked = false;
    sl.rtt_eligible = true;
    sl.fast_rexmitted = false;
    transmit_data(snd_next_, now_us);
    queued_offset_ += sl.len;
    ++snd_next_;
  }
  if (stalled) ++st_.window_stalls;
  arm_persist_if_stuck(now_us);
}

// The sender is "stuck" when nothing it owns will fire on its own: every packet
// in flight has been selectively acknowledged (so no RTO is pending) and the
// window leaves no room for new data. That happens when the peer's application
// stops reading: its cumulative ACK freezes while its window shrinks to zero.
// The only way out is a probe that provokes a fresh window advertisement.
void SrSender::arm_persist_if_stuck(std::uint64_t now_us) {
  if (state_ != State::Established || snd_base_ == final_seq_) {
    persist_deadline_us_ = UINT64_MAX;
    return;
  }
  bool have_running_timer = false;
  const std::int32_t span = seq_diff(snd_next_, snd_base_);
  for (std::int32_t i = 0; i < span; ++i) {
    const Slot& sl = slot(static_cast<seq_t>(snd_base_ + static_cast<seq_t>(i)));
    if (sl.in_use && !sl.acked) {
      have_running_timer = true;
      break;
    }
  }
  const bool can_send =
      queued_offset_ < len_ &&
      static_cast<std::uint32_t>(seq_diff(snd_next_, snd_base_)) < effective_window();
  if (have_running_timer || can_send) {
    persist_deadline_us_ = UINT64_MAX;
    persist_interval_us_ = cfg_.zero_window_probe_us;
    return;
  }
  if (persist_deadline_us_ == UINT64_MAX) persist_deadline_us_ = now_us + persist_interval_us_;
}

void SrSender::send_persist_probe(std::uint64_t now_us) {
  ++st_.zero_window_probes;
  if (in_flight() > 0) {
    // Re-send the oldest packet. The peer will discard it as a duplicate and
    // answer with an ACK that carries its current window, which is the whole
    // point; it is not a loss recovery and is never counted as one.
    Slot& sl = slot(snd_base_);
    sl.rtt_eligible = false;
    transmit_data(snd_base_, now_us);
  } else if (queued_offset_ < len_) {
    // Nothing outstanding at all: push one packet past the closed window.
    Slot& sl = slot(snd_next_);
    sl.offset = queued_offset_;
    sl.len = static_cast<std::uint16_t>(
        std::min<std::size_t>(len_ - queued_offset_, cfg_.max_payload));
    sl.retx = 0;
    sl.in_use = true;
    sl.acked = false;
    sl.rtt_eligible = false;  // a probe may legitimately be discarded
    sl.fast_rexmitted = false;
    transmit_data(snd_next_, now_us);
    queued_offset_ += sl.len;
    ++snd_next_;
  }
  // Exponential backoff, so a long application stall costs a handful of probes
  // rather than a steady stream of them.
  persist_interval_us_ = std::min(persist_interval_us_ * 2, cfg_.max_rto_us);
  persist_deadline_us_ = now_us + persist_interval_us_;
}

void SrSender::check_transfer_complete(std::uint64_t now_us) {
  if (state_ != State::Established) return;
  if (queued_offset_ == len_ && snd_base_ == final_seq_) {
    state_ = State::FinSent;
    control_retries_ = 0;
    send_control(PacketType::Fin, final_seq_, now_us);
  }
}

void SrSender::handle_ack(const Header& h, const SackBlock* sacks, std::size_t n,
                          std::uint64_t now_us) {
  ++st_.acks_received;
  rwnd_ = h.window;

  // RFC 6298 takes at most one RTT measurement per ACK. We record the best
  // candidate here and feed the estimator once, after the whole ACK is
  // processed. Karn's algorithm: only packets transmitted exactly once are
  // eligible, so an ACK arriving after a retransmission is never ambiguous.
  bool have_sample = false;
  std::uint64_t sample_us = 0;
  auto offer_sample = [&](std::uint64_t sent_us) {
    const std::uint64_t r = now_us - sent_us;
    if (!have_sample || r < sample_us) sample_us = r;
    have_sample = true;
  };

  // --- cumulative part -----------------------------------------------------
  bool advanced = false;
  if (seq_gt(h.ack, snd_base_) && seq_le(h.ack, snd_next_)) {
    advanced = true;
    for (seq_t s = snd_base_; seq_lt(s, h.ack); ++s) {
      Slot& sl = slot(s);
      if (sl.in_use) rack_xmit_us_ = std::max(rack_xmit_us_, sl.last_send_us);
      if (sl.in_use && !sl.acked && sl.rtt_eligible) offer_sample(sl.last_send_us);
      sl.in_use = false;
      sl.acked = false;
      sl.rtt_eligible = false;
      sl.fast_rexmitted = false;
    }
    snd_base_ = h.ack;
  } else if (h.ack == snd_base_) {
    ++st_.dup_acks;
  }

  // --- selective part ------------------------------------------------------
  st_.sack_blocks_received += n;
  for (std::size_t i = 0; i < n; ++i) {
    if (i == 0 && is_dsack(sacks, n, h.ack)) {
      ++st_.dsacks_received;
      if (reordering_seen_) {
        // Each further duplicate report means the previous window was still too
        // small to tell "late" from "lost" (RFC 8985 sec. 7.2).
        reorder_window_mult_ = std::min(reorder_window_mult_ + 1,
                                        cfg_.reorder_window_max_mult);
      }
      reordering_seen_ = true;
      continue;
    }
    seq_t s = sacks[i].start;
    // Guard against a peer (or a corrupted-but-checksum-valid replay) sending
    // an absurd range: iterate at most the window.
    for (std::uint32_t k = 0; k < cfg_.send_window && seq_lt(s, sacks[i].end); ++k, ++s) {
      if (!seq_in_range(s, snd_base_, snd_next_)) continue;
      Slot& sl = slot(s);
      if (sl.in_use && !sl.acked) {
        sl.acked = true;
        rack_xmit_us_ = std::max(rack_xmit_us_, sl.last_send_us);
        if (sl.rtt_eligible) {
          // A SACKed, never-retransmitted packet is an unambiguous sample too.
          offer_sample(sl.last_send_us);
          sl.rtt_eligible = false;
        }
      }
    }
  }

  if (have_sample) rtt_.on_sample(sample_us);

  if (advanced) {
    // Slide over any prefix that SACK already covered.
    while (seq_lt(snd_base_, snd_next_) && slot(snd_base_).in_use && slot(snd_base_).acked) {
      Slot& sl = slot(snd_base_);
      sl.in_use = false;
      sl.acked = false;
      sl.fast_rexmitted = false;
      ++snd_base_;
    }
  }

  detect_sack_losses(now_us);
  fill_window(now_us);
  check_transfer_complete(now_us);
}

// RFC 6675 style loss inference: a hole is declared lost once dup_sack_threshold
// packets with strictly higher sequence numbers have been acknowledged.
// This is what makes retransmission *selective* rather than go-back-N: only the
// specific holes are resent, never the packets above them.
//
// The extra age test is RFC 8985's idea: on a reordering path the packets above
// a hole get acknowledged simply because the hole is running late, and counting
// them alone turns every reordering event into a wasted retransmission. Waiting
// until the hole is also older than a fraction of the smoothed RTT separates
// "late" from "lost".
void SrSender::detect_sack_losses(std::uint64_t now_us) {
  if (state_ != State::Established) return;
  const std::int32_t span = seq_diff(snd_next_, snd_base_);
  if (span <= static_cast<std::int32_t>(cfg_.dup_sack_threshold)) return;

  std::uint64_t reorder_window_us = 0;
  if (reordering_seen_ && cfg_.reorder_window_divisor > 0 && rtt_.samples() > 0) {
    const double step = rtt_.srtt_us() / static_cast<double>(cfg_.reorder_window_divisor);
    reorder_window_us = std::min(static_cast<std::uint64_t>(step * reorder_window_mult_),
                                 static_cast<std::uint64_t>(rtt_.srtt_us()));
  }
  st_.reorder_window_us = reorder_window_us;

  std::uint32_t acked_above = 0;
  for (std::int32_t i = span - 1; i >= 0; --i) {
    const seq_t s = static_cast<seq_t>(snd_base_ + static_cast<seq_t>(i));
    Slot& sl = slot(s);
    if (!sl.in_use) continue;
    if (sl.acked) {
      ++acked_above;
      continue;
    }
    // RACK: compare transmission times, not wall-clock age. A hole sent at
    // essentially the same moment as the packets that were acknowledged is
    // still in flight, not lost.
    if (reorder_window_us > 0 && sl.last_send_us + reorder_window_us >= rack_xmit_us_) {
      continue;  // late, not lost
    }
    if (acked_above >= cfg_.dup_sack_threshold && !sl.fast_rexmitted) {
      sl.fast_rexmitted = true;
      sl.rtt_eligible = false;  // Karn
      ++sl.retx;
      ++st_.retransmits;
      ++st_.fast_retransmits;
      if (sl.retx > cfg_.max_retx_per_packet) {
        fail("packet exceeded max_retx_per_packet (fast path)");
        return;
      }
      transmit_data(s, now_us);
    }
  }
}

void SrSender::on_datagram(const std::uint8_t* buf, std::size_t len, std::uint64_t now_us) {
  Header h;
  SackBlock sacks[kMaxSackBlocks];
  std::size_t nsack = 0;
  const std::uint8_t* payload = nullptr;
  std::size_t payload_len = 0;

  const ParseResult pr = parse_packet(buf, len, h, sacks, nsack, payload, payload_len);
  switch (pr) {
    case ParseResult::Ok: break;
    case ParseResult::BadChecksum: ++st_.rx_bad_checksum; return;
    case ParseResult::BadVersion:  ++st_.rx_bad_version;  return;
    default:                       ++st_.rx_malformed;    return;
  }
  if (h.session_id != cfg_.session_id) {
    ++st_.rx_bad_session;
    return;
  }
  if (state_ == State::Closed || state_ == State::Failed) {
    ++st_.rx_ignored;
    return;
  }

  switch (h.type) {
    case PacketType::SynAck:
      if (state_ == State::SynSent) {
        if (h.ack != isn_) { ++st_.rx_ignored; return; }
        if (syn_rtt_eligible_) rtt_.on_sample(now_us - syn_send_us_);
        rwnd_ = h.window;
        state_ = State::Established;
        control_deadline_us_ = UINT64_MAX;
        control_retries_ = 0;
        fill_window(now_us);
        check_transfer_complete(now_us);
      } else {
        ++st_.rx_ignored;  // duplicate SYN_ACK
      }
      return;

    case PacketType::Ack:
      if (state_ == State::Established || state_ == State::FinSent) {
        handle_ack(h, sacks, nsack, now_us);
      } else {
        ++st_.rx_ignored;
      }
      return;

    case PacketType::FinAck:
      if (state_ == State::FinSent && h.ack == final_seq_) {
        state_ = State::Closed;
        control_deadline_us_ = UINT64_MAX;
      } else {
        ++st_.rx_ignored;
      }
      return;

    case PacketType::Rst:
      if (state_ == State::FinSent) {
        // The peer has forgotten the session. FIN is only ever sent after every
        // data packet was cumulatively acknowledged, so the peer had the whole
        // stream before it dropped the state: this is a completed transfer
        // whose FIN_ACK was lost, not a failure.
        state_ = State::Closed;
        control_deadline_us_ = UINT64_MAX;
      } else {
        fail("peer sent RST");
      }
      return;

    default:
      ++st_.rx_ignored;
      return;
  }
}

void SrSender::tick(std::uint64_t now_us) {
  if (state_ == State::Closed || state_ == State::Failed || state_ == State::Idle) return;

  if (state_ == State::SynSent || state_ == State::FinSent) {
    if (now_us >= control_deadline_us_) {
      if (control_retries_ >= cfg_.handshake_retries) {
        fail(state_ == State::SynSent ? "handshake timed out" : "FIN timed out");
        return;
      }
      ++control_retries_;
      rtt_.on_timeout();
      if (state_ == State::SynSent) {
        syn_rtt_eligible_ = false;  // Karn: the SYN is now ambiguous
        send_control(PacketType::Syn, isn_, now_us);
      } else {
        send_control(PacketType::Fin, final_seq_, now_us);
      }
    }
  }

  if (state_ == State::Established) {
    const std::int32_t span = seq_diff(snd_next_, snd_base_);
    bool base_timed_out = false;
    for (std::int32_t i = 0; i < span; ++i) {
      const seq_t s = static_cast<seq_t>(snd_base_ + static_cast<seq_t>(i));
      Slot& sl = slot(s);
      if (!sl.in_use || sl.acked) continue;
      if (now_us < sl.rto_deadline_us) continue;
      if (i == 0) base_timed_out = true;
      ++sl.retx;
      sl.rtt_eligible = false;  // Karn
      // fast_rexmitted is deliberately left set: each packet gets at most one
      // SACK-triggered retransmission, everything after that is timer-driven.
      sl.fast_rexmitted = true;
      ++st_.retransmits;
      ++st_.timeout_retransmits;
      if (sl.retx > cfg_.max_retx_per_packet) {
        fail("packet exceeded max_retx_per_packet");
        return;
      }
      transmit_data(s, now_us);
    }
    // Back the RTO off once per timeout event, driven by the oldest packet,
    // mirroring RFC 6298's single connection-level retransmission timer.
    if (base_timed_out) rtt_.on_timeout();

    if (persist_deadline_us_ != UINT64_MAX && now_us >= persist_deadline_us_) {
      send_persist_probe(now_us);
    }

    fill_window(now_us);
    check_transfer_complete(now_us);
    arm_persist_if_stuck(now_us);
  }
}

std::uint64_t SrSender::next_deadline_us() const {
  if (state_ == State::Closed || state_ == State::Failed || state_ == State::Idle) {
    return UINT64_MAX;
  }
  std::uint64_t d = UINT64_MAX;
  if (state_ == State::SynSent || state_ == State::FinSent) {
    d = std::min(d, control_deadline_us_);
  }
  if (state_ == State::Established) {
    const std::int32_t span = seq_diff(snd_next_, snd_base_);
    for (std::int32_t i = 0; i < span; ++i) {
      const Slot& sl = slot(static_cast<seq_t>(snd_base_ + static_cast<seq_t>(i)));
      if (sl.in_use && !sl.acked) d = std::min(d, sl.rto_deadline_us);
    }
    d = std::min(d, persist_deadline_us_);
  }
  return d;
}

}  // namespace rudp
