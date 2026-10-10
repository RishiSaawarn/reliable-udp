#include "reliable_udp/receiver.hpp"

#include <algorithm>
#include <cstring>

namespace rudp {

SrReceiver::SrReceiver(const Config& cfg, DatagramSink& sink, DeliverFn deliver)
    : cfg_(Config(cfg).normalize()),
      sink_(sink),
      deliver_(std::move(deliver)),
      slots_(cfg_.recv_window) {
  mask_ = cfg_.recv_window - 1;
  // Preallocate every reassembly slot up front so the packet path never
  // allocates once the session is running.
  for (Slot& s : slots_) s.buf.resize(cfg_.max_payload);
}

const char* SrReceiver::state_name(State s) {
  switch (s) {
    case State::Listen:      return "LISTEN";
    case State::Established: return "ESTABLISHED";
    case State::Closing:     return "CLOSING";
    case State::Closed:      return "CLOSED";
  }
  return "?";
}

std::uint32_t SrReceiver::advertised_window() const {
  // The acceptance range is [rcv_base, rcv_base + recv_window). Slots already
  // holding out-of-order data cannot take new packets, so the free capacity is
  // what we advertise. This is the flow-control signal the sender obeys.
  return cfg_.recv_window - buffered_;
}

std::size_t SrReceiver::build_sacks(SackBlock* out, std::size_t max_blocks) const {
  if (buffered_ == 0 || max_blocks == 0) return 0;
  std::size_t n = 0;
  // Normally rcv_base is never present -- it would have been delivered on
  // arrival -- but while the application is paused it can be, and reporting it
  // is accurate information the sender can act on.
  std::uint32_t i = 0;
  while (i < cfg_.recv_window && n < max_blocks) {
    const seq_t s = static_cast<seq_t>(rcv_base_ + i);
    if (!slots_[s & mask_].present) { ++i; continue; }
    const seq_t start = s;
    while (i < cfg_.recv_window &&
           slots_[static_cast<seq_t>(rcv_base_ + i) & mask_].present) {
      ++i;
    }
    out[n].start = start;
    out[n].end = static_cast<seq_t>(rcv_base_ + i);
    ++n;
  }
  return n;
}

void SrReceiver::note_duplicate(seq_t seq) {
  ++st_.dup_data;
  dsack_pending_ = true;
  dsack_seq_ = seq;
}

void SrReceiver::send_ack(PacketType type, std::uint64_t now_us) {
  SackBlock sacks[kMaxSackBlocks];
  std::size_t n = 0;
  if (type == PacketType::Ack) {
    if (dsack_pending_) {
      // RFC 2883 puts the D-SACK block first; it may sit entirely below the
      // cumulative ACK, which is exactly what marks it as a duplicate report.
      sacks[0].start = dsack_seq_;
      sacks[0].end = static_cast<seq_t>(dsack_seq_ + 1);
      n = 1;
      dsack_pending_ = false;
      ++st_.dsacks_sent;
    }
    n += build_sacks(sacks + n, kMaxSackBlocks - n);
  }

  Header h;
  h.type = type;
  h.session_id = session_id_;
  h.seq = rcv_base_;
  h.ack = rcv_base_;
  const std::uint32_t win = advertised_window();
  h.window = static_cast<std::uint16_t>(std::min<std::uint32_t>(win, 0xFFFFu));
  if (win == 0) ++st_.zero_window_advertised;

  const std::size_t bytes = encode_packet(head_buf_, h, sacks, n, nullptr, 0);
  sink_.send_datagram(head_buf_, bytes, nullptr, 0);
  if (type == PacketType::Ack) {
    ++st_.acks_sent;
    st_.sack_blocks_sent += n;
  }
  unacked_since_ = 0;
  ack_deadline_us_ = UINT64_MAX;
  (void)now_us;
}

void SrReceiver::set_app_paused(bool paused, std::uint64_t now_us) {
  const bool was = app_paused_;
  app_paused_ = paused;
  if (was && !paused && state_ != State::Closed) {
    deliver_contiguous();
    if (fin_seen_ && rcv_base_ == fin_seq_) {
      send_ack(PacketType::FinAck, now_us);
      if (state_ != State::Closing) {
        state_ = State::Closing;
        close_deadline_us_ = now_us + cfg_.linger_us;
      }
    } else if (state_ == State::Established) {
      send_ack(PacketType::Ack, now_us);  // window update
    }
  }
}

void SrReceiver::deliver_contiguous() {
  if (app_paused_) return;
  while (true) {
    Slot& s = slot(rcv_base_);
    if (!s.present) break;
    if (s.len > 0) {
      deliver_(s.buf.data(), s.len);
      st_.data_bytes_delivered += s.len;
    }
    s.present = false;
    s.len = 0;
    --buffered_;
    ++rcv_base_;
  }
}

void SrReceiver::on_datagram(const std::uint8_t* buf, std::size_t len, std::uint64_t now_us) {
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

  if (state_ == State::Listen) {
    if (h.type != PacketType::Syn) {
      // Data for a session we never accepted: ignore rather than guess.
      ++st_.rx_ignored;
      return;
    }
    session_id_ = h.session_id;
    isn_ = h.seq;
    rcv_base_ = h.seq;
    state_ = State::Established;
    send_ack(PacketType::SynAck, now_us);
    return;
  }

  if (h.session_id != session_id_) {
    ++st_.rx_bad_session;
    return;
  }
  if (state_ == State::Closed) {
    // A retransmitted FIN still gets answered: our peer only retries because
    // its FIN_ACK was lost, and leaving it unanswered would make it burn its
    // whole retry budget and declare a successful transfer failed. This is the
    // same role TCP's TIME_WAIT plays; the difference is that this endpoint is
    // owned by the caller, so answering costs nothing and needs no new state.
    if (h.type == PacketType::Fin && h.seq == fin_seq_) {
      send_ack(PacketType::FinAck, now_us);
    } else {
      ++st_.rx_ignored;
    }
    return;
  }

  switch (h.type) {
    case PacketType::Syn:
      // Retransmitted or duplicated SYN. Re-answer without touching state:
      // the session is already established and rcv_base must not move.
      if (h.seq == isn_) {
        send_ack(PacketType::SynAck, now_us);
      } else {
        ++st_.rx_ignored;
      }
      return;

    case PacketType::Data: {
      ++st_.data_packets_received;
      if (payload_len > cfg_.max_payload) {  // peer using a larger MTU than us
        ++st_.rx_malformed;
        return;
      }
      const std::int32_t d = seq_diff(h.seq, rcv_base_);
      if (d < 0) {
        // Below the base. Within one window back it is a recent duplicate --
        // a retransmission whose ACK was lost, or a reordered original -- and
        // is worth reporting as a D-SACK. Further back than that we claim no
        // knowledge: it is simply outside the window and gets discarded.
        if (d >= -static_cast<std::int32_t>(cfg_.recv_window)) {
          note_duplicate(h.seq);
        } else {
          ++st_.out_of_window;
        }
      } else if (d >= static_cast<std::int32_t>(cfg_.recv_window)) {
        ++st_.out_of_window;
      } else {
        Slot& s = slot(h.seq);
        if (s.present) {
          note_duplicate(h.seq);
        } else {
          if (payload_len > 0) std::memcpy(s.buf.data(), payload, payload_len);
          s.len = static_cast<std::uint16_t>(payload_len);
          s.present = true;
          ++buffered_;
          if (d > 0) ++st_.out_of_order;
          st_.max_buffered = std::max<std::uint64_t>(st_.max_buffered, buffered_);
          deliver_contiguous();
        }
      }

      if (fin_seen_ && rcv_base_ == fin_seq_) {
        send_ack(PacketType::FinAck, now_us);
        state_ = State::Closing;
        close_deadline_us_ = now_us + cfg_.linger_us;
        return;
      }
      // Delayed-ACK policy. With ack_every == 1 (the default) this is an
      // immediate ACK per DATA packet and the timer is never armed.
      ++unacked_since_;
      if (unacked_since_ >= cfg_.ack_every || d != 0 || buffered_ > 0) {
        send_ack(PacketType::Ack, now_us);
      } else if (ack_deadline_us_ == UINT64_MAX) {
        ack_deadline_us_ = now_us + cfg_.ack_delay_us;
      }
      return;
    }

    case PacketType::Fin:
      fin_seen_ = true;
      fin_seq_ = h.seq;
      if (rcv_base_ == fin_seq_) {
        send_ack(PacketType::FinAck, now_us);
        // Restart the linger window on every FIN, so a peer that is still
        // retrying always finds us willing to answer.
        state_ = State::Closing;
        close_deadline_us_ = now_us + cfg_.linger_us;
      } else {
        // Holes remain; a plain ACK tells the sender exactly what is missing.
        send_ack(PacketType::Ack, now_us);
      }
      return;

    default:
      ++st_.rx_ignored;
      return;
  }
}

void SrReceiver::tick(std::uint64_t now_us) {
  if (state_ == State::Closed) return;
  if (ack_deadline_us_ != UINT64_MAX && now_us >= ack_deadline_us_) {
    send_ack(PacketType::Ack, now_us);
  }
  if (state_ == State::Closing && now_us >= close_deadline_us_) {
    state_ = State::Closed;
    close_deadline_us_ = UINT64_MAX;
  }
}

std::uint64_t SrReceiver::next_deadline_us() const {
  std::uint64_t d = UINT64_MAX;
  if (state_ == State::Closed) return d;
  d = std::min(d, ack_deadline_us_);
  if (state_ == State::Closing) d = std::min(d, close_deadline_us_);
  return d;
}

}  // namespace rudp
