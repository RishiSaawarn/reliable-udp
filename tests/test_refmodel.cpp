// Differential testing against the independent reference model in ref_model.hpp.
#include <cstring>
#include <random>
#include <set>
#include <vector>

#include "ref_model.hpp"
#include "reliable_udp/receiver.hpp"
#include "reliable_udp/sender.hpp"
#include "reliable_udp/sim/sim_harness.hpp"
#include "test_framework.hpp"

using namespace rudp;
using namespace rudp::sim;

namespace {

class RecordingSink final : public DatagramSink {
 public:
  void send_datagram(const std::uint8_t* head, std::size_t hl, const std::uint8_t* pay,
                     std::size_t pl) override {
    std::vector<std::uint8_t> v(hl + pl);
    std::memcpy(v.data(), head, hl);
    if (pl) std::memcpy(v.data() + hl, pay, pl);
    out.push_back(std::move(v));
  }
  void clear() { out.clear(); }
  std::vector<std::vector<std::uint8_t>> out;
};

struct Decoded {
  Header h;
  SackBlock sacks[kMaxSackBlocks];
  std::size_t nsack = 0;
  const std::uint8_t* payload = nullptr;
  std::size_t plen = 0;
  ParseResult r = ParseResult::Ok;
};

Decoded decode(const std::vector<std::uint8_t>& b) {
  Decoded d;
  d.r = parse_packet(b.data(), b.size(), d.h, d.sacks, d.nsack, d.payload, d.plen);
  return d;
}

std::vector<std::uint8_t> make_data_pkt(std::uint32_t sid, seq_t seq,
                                        const std::uint8_t* p, std::size_t n) {
  Header h;
  h.type = PacketType::Data;
  h.session_id = sid;
  h.seq = seq;
  std::vector<std::uint8_t> buf(kMaxOverhead + n);
  const std::size_t hl = encode_packet(buf.data(), h, nullptr, 0, p, n);
  if (n) std::memcpy(buf.data() + hl, p, n);
  buf.resize(hl + n);
  return buf;
}

}  // namespace

// -------------------------------------------------------------- receiver A/B
TEST(refmodel, "receiver matches the model over 200k randomised arrivals") {
  std::mt19937_64 rng(0xBEEF2026);
  for (int scenario = 0; scenario < 40; ++scenario) {
    Config c;
    c.max_payload = static_cast<std::uint16_t>(16 + (rng() % 64));
    c.recv_window = 1u << (1 + (rng() % 6));  // 2 .. 64
    c.send_window = c.recv_window;
    c.session_id = 0x11223344;
    // Alternate between an ordinary ISN and one that forces a wrap.
    c.isn = (scenario % 2 == 0) ? static_cast<seq_t>(rng())
                                : static_cast<seq_t>(0xFFFFFFFFu - (rng() % 16));
    c.normalize();

    RecordingSink sink;
    std::vector<std::uint8_t> got;
    SrReceiver real(c, sink, [&](const std::uint8_t* p, std::size_t n) {
      got.insert(got.end(), p, p + n);
    });
    refmodel::RefReceiver model(c.isn, c.recv_window, c.max_payload);

    // Handshake, which the model does not need to represent.
    {
      Header h;
      h.type = PacketType::Syn;
      h.session_id = c.session_id;
      h.seq = c.isn;
      std::vector<std::uint8_t> syn(kMaxOverhead);
      syn.resize(encode_packet(syn.data(), h, nullptr, 0, nullptr, 0));
      real.on_datagram(syn.data(), syn.size(), 0);
    }

    // A deterministic stream of arrivals: in-order, out-of-order, duplicated,
    // below the base and above the window, in randomised proportions.
    const int kEvents = 5000;
    for (int e = 0; e < kEvents; ++e) {
      const seq_t base = real.rcv_base();
      seq_t seq;
      const std::uint64_t roll = rng() % 100;
      if (roll < 45) {
        seq = static_cast<seq_t>(base + (rng() % c.recv_window));
      } else if (roll < 65) {
        seq = base;
      } else if (roll < 80) {
        seq = static_cast<seq_t>(base - 1 - (rng() % 32));   // already delivered
      } else if (roll < 90) {
        seq = static_cast<seq_t>(base + c.recv_window + (rng() % 64));  // above window
      } else {
        seq = static_cast<seq_t>(rng());                     // wild
      }
      const std::size_t plen = rng() % (c.max_payload + 1);
      std::vector<std::uint8_t> payload(plen);
      // Content is a pure function of the sequence number, so a mis-indexed
      // buffer would show up as a content mismatch, not just a length one.
      for (std::size_t i = 0; i < plen; ++i) {
        payload[i] = static_cast<std::uint8_t>((seq * 131u + i * 17u) & 0xFF);
      }
      const auto pkt = make_data_pkt(c.session_id, seq, payload.data(), plen);

      sink.clear();
      real.on_datagram(pkt.data(), pkt.size(), static_cast<std::uint64_t>(e));
      model.on_data(seq, payload.data(), plen);

      CHECK_MSG(real.rcv_base() == model.rcv_base(),
                "rcv_base diverged at scenario " + std::to_string(scenario) + " event " +
                    std::to_string(e) + ": real=" + std::to_string(real.rcv_base()) +
                    " model=" + std::to_string(model.rcv_base()));
      CHECK_MSG(real.buffered() == model.buffered(),
                "buffered count diverged at event " + std::to_string(e));
      CHECK_MSG(real.advertised_window() == model.advertised_window(),
                "advertised window diverged at event " + std::to_string(e));
      CHECK_MSG(got == model.delivered(),
                "delivered stream diverged at scenario " + std::to_string(scenario) +
                    " event " + std::to_string(e));

      // The ACK the real receiver emitted must carry exactly the SACK blocks
      // the model derives from its own buffer.
      CHECK(!sink.out.empty());
      const Decoded d = decode(sink.out.back());
      CHECK_EQ(static_cast<int>(d.r), static_cast<int>(ParseResult::Ok));
      if (d.h.type == PacketType::Ack) {
        CHECK_EQ(d.h.ack, model.rcv_base());
        const auto want = model.expected_ack_blocks(kMaxSackBlocks);
        CHECK_MSG(d.nsack == want.size(),
                  "sack block count diverged at scenario " + std::to_string(scenario) +
                      " event " + std::to_string(e) + ": real=" + std::to_string(d.nsack) +
                      " model=" + std::to_string(want.size()) + " (seq=" +
                      std::to_string(seq) + " rcv_base=" + std::to_string(real.rcv_base()) +
                      ")");
        for (std::size_t i = 0; i < want.size(); ++i) {
          CHECK_EQ(d.sacks[i].start, want[i].start);
          CHECK_EQ(d.sacks[i].end, want[i].end);
        }
      }
    }
  }
}

// ---------------------------------------------------------------- sender A/B
TEST(refmodel, "sender obeys every window and retransmission rule the model checks") {
  std::mt19937_64 rng(0x5EED2026);
  for (int scenario = 0; scenario < 60; ++scenario) {
    Config c;
    c.max_payload = static_cast<std::uint16_t>(32 + (rng() % 200));
    c.send_window = 1u << (1 + (rng() % 6));
    c.recv_window = c.send_window * 2;
    c.session_id = 0x0BADF00D;
    c.isn = (scenario % 3 == 0) ? static_cast<seq_t>(0xFFFFFF80u) : static_cast<seq_t>(rng());
    c.min_rto_us = 1000;
    c.init_rto_us = 5000;
    c.max_retx_per_packet = 1000;
    c.normalize();

    const std::size_t nbytes = 1 + (rng() % 40000);
    std::vector<std::uint8_t> data(nbytes);
    fill_pattern(data, rng());
    const std::int64_t total_pkts =
        static_cast<std::int64_t>((nbytes + c.max_payload - 1) / c.max_payload);

    RecordingSink sink;
    SrSender real(c, sink, data.data(), data.size());
    refmodel::RefSender model(c.isn, total_pkts, c.send_window);

    std::uint64_t now = 0;
    real.start(now);

    // SYN_ACK so the sender may start sending.
    {
      Header h;
      h.type = PacketType::SynAck;
      h.session_id = c.session_id;
      h.ack = c.isn;
      h.window = static_cast<std::uint16_t>(c.recv_window);
      std::vector<std::uint8_t> sa(kMaxOverhead);
      sa.resize(encode_packet(sa.data(), h, nullptr, 0, nullptr, 0));
      sink.clear();
      real.on_datagram(sa.data(), sa.size(), now);
    }

    std::set<std::int64_t> ever_sent;
    seq_t pending_fin = 0;
    bool have_fin = false;
    seq_t cum = c.isn;
    std::set<std::int64_t> peer_have;  // indices the notional peer received

    auto drain = [&]() {
      for (const auto& pkt : sink.out) {
        const Decoded d = decode(pkt);
        CHECK_EQ(static_cast<int>(d.r), static_cast<int>(ParseResult::Ok));
        if (d.h.type == PacketType::Fin) {
          pending_fin = d.h.seq;
          have_fin = true;
          continue;
        }
        if (d.h.type != PacketType::Data) continue;
        const char* err = model.note_transmit(d.h.seq);
        CHECK_MSG(err == nullptr,
                  std::string("scenario ") + std::to_string(scenario) + ": " +
                      (err ? err : "") + " (seq=" + std::to_string(d.h.seq) + ")");
        const std::int64_t idx =
            static_cast<std::int64_t>(static_cast<std::int32_t>(d.h.seq - c.isn));
        ever_sent.insert(idx);
        // The payload must be the right slice of the source stream.
        CHECK(idx >= 0);
        const std::size_t off = static_cast<std::size_t>(idx) * c.max_payload;
        CHECK(off < data.size());
        const std::size_t want_len = std::min<std::size_t>(c.max_payload, data.size() - off);
        CHECK_EQ(d.plen, want_len);
        CHECK(std::memcmp(d.payload, data.data() + off, want_len) == 0);
        // A randomly chosen subset of transmissions "arrives".
        if (rng() % 100 < 75) peer_have.insert(idx);
      }
      sink.clear();
    };
    drain();

    for (int step = 0; step < 4000 && !real.done() && !real.failed(); ++step) {
      // Build the ACK the notional peer would send from what it has.
      std::int64_t base_idx = 0;
      while (peer_have.count(base_idx) != 0) ++base_idx;
      cum = static_cast<seq_t>(c.isn + static_cast<seq_t>(base_idx));

      std::vector<SackBlock> sacks;
      {
        auto it = peer_have.lower_bound(base_idx);
        while (it != peer_have.end() && sacks.size() < kMaxSackBlocks) {
          const std::int64_t s = *it;
          std::int64_t e = s + 1;
          ++it;
          while (it != peer_have.end() && *it == e) { ++e; ++it; }
          sacks.push_back(SackBlock{static_cast<seq_t>(c.isn + static_cast<seq_t>(s)),
                                    static_cast<seq_t>(c.isn + static_cast<seq_t>(e))});
        }
      }

      Header h;
      h.type = PacketType::Ack;
      h.session_id = c.session_id;
      h.ack = cum;
      h.window = static_cast<std::uint16_t>(c.recv_window);
      std::vector<std::uint8_t> ack(kMaxOverhead);
      ack.resize(encode_packet(ack.data(), h, sacks.data(), sacks.size(), nullptr, 0));

      if (rng() % 100 < 80) {  // 20% of ACKs are lost
        real.on_datagram(ack.data(), ack.size(), now);
        model.note_ack(cum, sacks.data(), sacks.size());
      }
      drain();

      // Answer a FIN once the notional peer has the whole stream, so the
      // session terminates instead of exhausting its FIN retry budget.
      if (have_fin) {
        Header fh;
        fh.type = PacketType::FinAck;
        fh.session_id = c.session_id;
        fh.ack = pending_fin;
        std::vector<std::uint8_t> fa(kMaxOverhead);
        fa.resize(encode_packet(fa.data(), fh, nullptr, 0, nullptr, 0));
        real.on_datagram(fa.data(), fa.size(), now);
        have_fin = false;
        drain();
      }

      CHECK_MSG(real.snd_base() == model.snd_base(),
                "snd_base diverged: real=" + std::to_string(real.snd_base()) +
                    " model=" + std::to_string(model.snd_base()));
      CHECK_MSG(real.in_flight() <= c.send_window, "in-flight exceeded the send window");

      now += 1000 + (rng() % 20000);  // advance time, sometimes past the RTO
      real.tick(now);
      drain();
    }

    CHECK_MSG(!real.failed(), std::string("sender failed: ") + real.failure_reason());
    CHECK_MSG(real.done(), "sender never reached CLOSED");
    // The peer's view converged, so the sender must have finished and every
    // packet in the stream must have been transmitted at least once.
    CHECK_MSG(static_cast<std::int64_t>(ever_sent.size()) == total_pkts,
              "not every packet was transmitted: " + std::to_string(ever_sent.size()) +
                  " of " + std::to_string(total_pkts));
  }
}

// ------------------------------------------- whole-transfer differential test
TEST(refmodel, "full transfers agree with the model fed the ground-truth trace") {
  std::mt19937_64 rng(0xD1FFED);
  for (int i = 0; i < 40; ++i) {
    Config c;
    c.max_payload = static_cast<std::uint16_t>(200 + (rng() % 1200));
    c.send_window = 1u << (2 + (rng() % 5));
    c.recv_window = c.send_window * 2;
    c.min_rto_us = 3000;
    c.linger_us = 30000;
    c.isn = (i % 4 == 0) ? 0xFFFFFF00u : static_cast<seq_t>(rng());
    c.normalize();

    SimParams p;
    p.cfg = c;
    p.seed = rng();
    p.fwd.loss_prob = 0.02 + static_cast<double>(rng() % 15) / 100.0;
    p.fwd.dup_prob = static_cast<double>(rng() % 10) / 100.0;
    p.fwd.reorder_prob = static_cast<double>(rng() % 25) / 100.0;
    p.fwd.delay_us = 1000 + (rng() % 8000);
    p.fwd.reorder_extra_us = p.fwd.delay_us;
    p.rev.loss_prob = static_cast<double>(rng() % 10) / 100.0;
    p.rev.delay_us = p.fwd.delay_us;

    std::vector<std::uint8_t> data(1 + (rng() % 150000));
    fill_pattern(data, rng());

    refmodel::RefReceiver model(c.isn, c.recv_window, c.max_payload);
    std::uint64_t data_arrivals = 0;
    p.fwd_tap = [&](const std::uint8_t* b, std::size_t n, std::uint64_t) {
      Header h;
      SackBlock s[kMaxSackBlocks];
      std::size_t ns = 0;
      const std::uint8_t* pay = nullptr;
      std::size_t pl = 0;
      if (parse_packet(b, n, h, s, ns, pay, pl) != ParseResult::Ok) return;  // corrupt
      if (h.type != PacketType::Data) return;
      ++data_arrivals;
      model.on_data(h.seq, pay, pl);
    };

    std::vector<std::uint8_t> out;
    auto rep = run_sim_transfer(data, p, &out);
    CHECK_MSG(rep.completed, "run " + std::to_string(i) + " failed: " + rep.failure);
    CHECK(data_arrivals > 0);
    // The model, driven only by the wire trace, must reconstruct exactly what
    // the real receiver handed to the application.
    CHECK_MSG(model.delivered() == out,
              "run " + std::to_string(i) + ": model delivered " +
                  std::to_string(model.delivered().size()) + " bytes, implementation " +
                  std::to_string(out.size()));
    CHECK_MSG(out == data, "run " + std::to_string(i) + ": output differs from source");
  }
}
