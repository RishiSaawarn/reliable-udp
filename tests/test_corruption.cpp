// Integrity and hostile-input handling. Anything that is not a well-formed,
// checksum-valid packet for this session must be counted and dropped, never
// acted on and never able to crash or corrupt the receiver.
#include <cstring>
#include <random>
#include <vector>

#include "reliable_udp/packet.hpp"
#include "reliable_udp/receiver.hpp"
#include "reliable_udp/sender.hpp"
#include "reliable_udp/sim/sim_harness.hpp"
#include "test_framework.hpp"

using namespace rudp;
using namespace rudp::sim;

namespace {

class CapturingSink final : public DatagramSink {
 public:
  void send_datagram(const std::uint8_t* head, std::size_t hl, const std::uint8_t* pay,
                     std::size_t pl) override {
    std::vector<std::uint8_t> v(hl + pl);
    std::memcpy(v.data(), head, hl);
    if (pl) std::memcpy(v.data() + hl, pay, pl);
    out.push_back(std::move(v));
  }
  std::vector<std::vector<std::uint8_t>> out;
};

Config cfg_for_tests() {
  Config c;
  c.max_payload = 64;
  c.send_window = 8;
  c.recv_window = 8;
  c.session_id = 0xABCD1234;
  c.isn = 1000;
  return c.normalize();
}

std::vector<std::uint8_t> pkt(PacketType t, std::uint32_t sid, seq_t seq, std::size_t plen,
                              std::uint32_t ack = 0) {
  Header h;
  h.type = t;
  h.session_id = sid;
  h.seq = seq;
  h.ack = ack;
  std::vector<std::uint8_t> payload(plen, 0x5A);
  std::vector<std::uint8_t> buf(kMaxOverhead + plen);
  const std::size_t hl =
      encode_packet(buf.data(), h, nullptr, 0, payload.empty() ? nullptr : payload.data(), plen);
  if (plen) std::memcpy(buf.data() + hl, payload.data(), plen);
  buf.resize(hl + plen);
  return buf;
}

// Brings a receiver up to ESTABLISHED with a SYN.
void establish(SrReceiver& r, const Config& c) {
  const auto syn = pkt(PacketType::Syn, c.session_id, c.isn, 0);
  r.on_datagram(syn.data(), syn.size(), 0);
  CHECK(r.state() == SrReceiver::State::Established);
}

}  // namespace

TEST(corrupt, "a corrupted header is rejected as a bad checksum") {
  Config c = cfg_for_tests();
  CapturingSink sink;
  SrReceiver r(c, sink, [](const std::uint8_t*, std::size_t) { CHECK(false); });
  establish(r, c);
  auto p = pkt(PacketType::Data, c.session_id, c.isn, 64);
  p[9] ^= 0x01;  // flip a bit in the sequence number
  r.on_datagram(p.data(), p.size(), 1);
  CHECK_EQ(r.stats().rx_bad_checksum, 1u);
  CHECK_EQ(r.stats().data_packets_received, 0u);
}

TEST(corrupt, "a corrupted payload is rejected as a bad checksum") {
  Config c = cfg_for_tests();
  CapturingSink sink;
  SrReceiver r(c, sink, [](const std::uint8_t*, std::size_t) { CHECK(false); });
  establish(r, c);
  auto p = pkt(PacketType::Data, c.session_id, c.isn, 64);
  p[kHeaderSize + 30] ^= 0x80;
  r.on_datagram(p.data(), p.size(), 1);
  CHECK_EQ(r.stats().rx_bad_checksum, 1u);
}

TEST(corrupt, "a corrupted checksum field itself is rejected") {
  Config c = cfg_for_tests();
  CapturingSink sink;
  SrReceiver r(c, sink, [](const std::uint8_t*, std::size_t) { CHECK(false); });
  establish(r, c);
  auto p = pkt(PacketType::Data, c.session_id, c.isn, 64);
  p[kChecksumOffset + 2] ^= 0x40;
  r.on_datagram(p.data(), p.size(), 1);
  CHECK_EQ(r.stats().rx_bad_checksum, 1u);
}

TEST(corrupt, "a packet for a different session is counted and ignored") {
  Config c = cfg_for_tests();
  CapturingSink sink;
  std::size_t delivered = 0;
  SrReceiver r(c, sink, [&](const std::uint8_t*, std::size_t n) { delivered += n; });
  establish(r, c);
  const auto p = pkt(PacketType::Data, 0xDEADBEEF, c.isn, 64);
  r.on_datagram(p.data(), p.size(), 1);
  CHECK_EQ(r.stats().rx_bad_session, 1u);
  CHECK_EQ(delivered, static_cast<std::size_t>(0));
  CHECK_EQ(r.stats().data_packets_received, 0u);
}

TEST(corrupt, "sequence numbers outside the receive window are rejected") {
  Config c = cfg_for_tests();
  CapturingSink sink;
  std::size_t delivered = 0;
  SrReceiver r(c, sink, [&](const std::uint8_t*, std::size_t n) { delivered += n; });
  establish(r, c);
  // Far above the window.
  auto hi = pkt(PacketType::Data, c.session_id, c.isn + 100000, 64);
  r.on_datagram(hi.data(), hi.size(), 1);
  CHECK_EQ(r.stats().out_of_window, 1u);
  // Far below the base: too old to reason about, so it is out of window rather
  // than a duplicate we are willing to acknowledge as one.
  auto ancient = pkt(PacketType::Data, c.session_id, c.isn - 5000, 64);
  r.on_datagram(ancient.data(), ancient.size(), 2);
  CHECK_EQ(r.stats().out_of_window, 2u);
  CHECK_EQ(r.stats().dup_data, 0u);
  // Just below the base is a recent duplicate.
  auto recent = pkt(PacketType::Data, c.session_id, c.isn - 1, 64);
  r.on_datagram(recent.data(), recent.size(), 3);
  CHECK_EQ(r.stats().dup_data, 1u);
  CHECK_EQ(delivered, static_cast<std::size_t>(0));
  CHECK(r.state() == SrReceiver::State::Established);
}

TEST(corrupt, "a payload larger than the local MTU is rejected, not overrun") {
  Config c = cfg_for_tests();  // max_payload = 64
  CapturingSink sink;
  SrReceiver r(c, sink, [](const std::uint8_t*, std::size_t) { CHECK(false); });
  establish(r, c);
  const auto big = pkt(PacketType::Data, c.session_id, c.isn, 4000);
  r.on_datagram(big.data(), big.size(), 1);
  CHECK_EQ(r.stats().rx_malformed, 1u);
}

TEST(corrupt, "data before a handshake is ignored") {
  Config c = cfg_for_tests();
  CapturingSink sink;
  SrReceiver r(c, sink, [](const std::uint8_t*, std::size_t) { CHECK(false); });
  const auto p = pkt(PacketType::Data, c.session_id, c.isn, 64);
  r.on_datagram(p.data(), p.size(), 1);
  CHECK_EQ(r.stats().rx_ignored, 1u);
  CHECK(r.state() == SrReceiver::State::Listen);
  CHECK(sink.out.empty());
}

TEST(corrupt, "a duplicated SYN re-answers without resetting the session") {
  Config c = cfg_for_tests();
  CapturingSink sink;
  std::size_t delivered = 0;
  SrReceiver r(c, sink, [&](const std::uint8_t*, std::size_t n) { delivered += n; });
  establish(r, c);
  const auto d = pkt(PacketType::Data, c.session_id, c.isn, 64);
  r.on_datagram(d.data(), d.size(), 1);
  CHECK_EQ(delivered, static_cast<std::size_t>(64));
  CHECK_EQ(r.rcv_base(), c.isn + 1);

  const auto syn = pkt(PacketType::Syn, c.session_id, c.isn, 0);
  r.on_datagram(syn.data(), syn.size(), 2);
  CHECK_EQ(r.rcv_base(), c.isn + 1);  // NOT rewound
  CHECK_EQ(delivered, static_cast<std::size_t>(64));
  // A SYN carrying a different ISN for an established session is nonsense.
  const auto bad_syn = pkt(PacketType::Syn, c.session_id, c.isn + 77, 0);
  r.on_datagram(bad_syn.data(), bad_syn.size(), 3);
  CHECK_EQ(r.rcv_base(), c.isn + 1);
  CHECK_EQ(r.stats().rx_ignored, 1u);
}

TEST(corrupt, "malformed and truncated datagrams never crash the receiver") {
  Config c = cfg_for_tests();
  CapturingSink sink;
  SrReceiver r(c, sink, [](const std::uint8_t*, std::size_t) {});
  establish(r, c);
  const auto valid = pkt(PacketType::Data, c.session_id, c.isn, 64);

  for (std::size_t n = 0; n <= valid.size(); ++n) {
    std::vector<std::uint8_t> t(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(n));
    r.on_datagram(t.empty() ? nullptr : t.data(), t.size(), 10);
  }
  std::vector<std::uint8_t> huge(60000, 0xFF);
  r.on_datagram(huge.data(), huge.size(), 11);
  CHECK(r.state() == SrReceiver::State::Established);
  CHECK(r.stats().rx_malformed + r.stats().rx_bad_checksum + r.stats().rx_bad_version > 0);
}

TEST(corrupt, "fuzzing the receiver with mutated packets is safe") {
  std::mt19937_64 rng(1234567);
  Config c = cfg_for_tests();
  for (int iter = 0; iter < 20000; ++iter) {
    CapturingSink sink;
    std::size_t delivered = 0;
    SrReceiver r(c, sink, [&](const std::uint8_t*, std::size_t n) { delivered += n; });
    establish(r, c);
    for (int k = 0; k < 8; ++k) {
      std::uniform_int_distribution<int> tsel(1, 7);
      std::uniform_int_distribution<std::size_t> psel(0, 70);
      auto p = pkt(static_cast<PacketType>(tsel(rng)), c.session_id,
                   static_cast<seq_t>(c.isn + (rng() % 32)), psel(rng));
      // Mutate 0-3 random bytes.
      const int nmut = static_cast<int>(rng() % 4);
      for (int m = 0; m < nmut; ++m) {
        p[rng() % p.size()] = static_cast<std::uint8_t>(rng() & 0xFF);
      }
      if (rng() % 8 == 0 && p.size() > 4) p.resize(rng() % p.size());
      r.on_datagram(p.data(), p.size(), 100 + static_cast<std::uint64_t>(k));
    }
    // Delivery must never exceed what the window could possibly hold.
    CHECK(delivered <= static_cast<std::size_t>(c.recv_window) * c.max_payload);
  }
}

TEST(corrupt, "fuzzing the sender with mutated packets is safe") {
  std::mt19937_64 rng(7654321);
  Config c = cfg_for_tests();
  std::vector<std::uint8_t> data(512, 0x11);
  for (int iter = 0; iter < 20000; ++iter) {
    CapturingSink sink;
    SrSender s(c, sink, data.data(), data.size());
    s.start(0);
    for (int k = 0; k < 8; ++k) {
      std::uniform_int_distribution<int> tsel(1, 7);
      auto p = pkt(static_cast<PacketType>(tsel(rng)), c.session_id,
                   static_cast<seq_t>(c.isn + (rng() % 32)), 0,
                   static_cast<seq_t>(c.isn + (rng() % 32)));
      const int nmut = static_cast<int>(rng() % 4);
      for (int m = 0; m < nmut; ++m) p[rng() % p.size()] = static_cast<std::uint8_t>(rng() & 0xFF);
      s.on_datagram(p.data(), p.size(), 100 + static_cast<std::uint64_t>(k));
      s.tick(200 + static_cast<std::uint64_t>(k) * 1000);
    }
    CHECK(s.in_flight() <= c.send_window);
  }
}

TEST(corrupt, "an on-wire corruption rate of 5%% still delivers the file intact") {
  std::vector<std::uint8_t> data(300 * 1024);
  fill_pattern(data, 99);
  SimParams p;
  p.cfg.max_payload = 1200;
  p.cfg.send_window = 32;
  p.cfg.recv_window = 64;
  p.cfg.min_rto_us = 5000;
  p.cfg.linger_us = 50000;
  p.cfg.normalize();
  p.fwd.delay_us = 3000;
  p.fwd.corrupt_prob = 0.05;
  p.rev.delay_us = 3000;
  p.rev.corrupt_prob = 0.05;
  p.seed = 4242;
  std::vector<std::uint8_t> out;
  auto rep = run_sim_transfer(data, p, &out);
  CHECK_MSG(rep.completed, "corrupted-link transfer failed: " + rep.failure);
  CHECK_EQ(rep.sha_sent, rep.sha_received);
  CHECK(out == data);
  CHECK(rep.fwd.corrupted > 5);
  CHECK(rep.rcv.rx_bad_checksum > 5);   // corruption was detected, not delivered
  CHECK(rep.snd.rx_bad_checksum > 0);
  CHECK(rep.snd.retransmits > 0);       // and it was recovered like loss
}
