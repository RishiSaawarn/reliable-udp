#include <cstring>
#include <random>
#include <vector>

#include "reliable_udp/crc32c.hpp"
#include "reliable_udp/packet.hpp"
#include "reliable_udp/sha256.hpp"
#include "test_framework.hpp"

using namespace rudp;

namespace {

std::vector<std::uint8_t> build(const Header& h, const SackBlock* s, std::size_t n,
                                const std::uint8_t* pay, std::size_t plen) {
  std::uint8_t head[kMaxOverhead];
  const std::size_t hl = encode_packet(head, h, s, n, pay, plen);
  std::vector<std::uint8_t> buf(head, head + hl);
  if (plen > 0) buf.insert(buf.end(), pay, pay + plen);
  return buf;
}

struct Parsed {
  ParseResult r;
  Header h;
  SackBlock sacks[kMaxSackBlocks];
  std::size_t nsack = 0;
  const std::uint8_t* payload = nullptr;
  std::size_t plen = 0;
};

// Mutates one byte, with an explicit bounds check so the compiler can see the
// buffer is non-empty.
void poke(std::vector<std::uint8_t>& b, std::size_t i, std::uint8_t v) {
  CHECK(i < b.size());
  b.data()[i] = v;
}

Parsed parse(const std::vector<std::uint8_t>& b) {
  Parsed p;
  p.r = parse_packet(b.data(), b.size(), p.h, p.sacks, p.nsack, p.payload, p.plen);
  return p;
}

}  // namespace

TEST(packet, "crc32c matches the published check value") {
  const char* s = "123456789";
  CHECK_EQ(crc32c(reinterpret_cast<const std::uint8_t*>(s), 9), 0xE3069283u);
  CHECK_EQ(crc32c(nullptr, 0), 0u);
}

TEST(packet, "sha256 matches published vectors") {
  CHECK_EQ(sha256_hex(nullptr, 0),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  const char* abc = "abc";
  CHECK_EQ(sha256_hex(reinterpret_cast<const std::uint8_t*>(abc), 3),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  std::vector<std::uint8_t> million(1000000, 'a');
  CHECK_EQ(sha256_hex(million.data(), million.size()),
           std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(packet, "header round-trips with payload and sack blocks") {
  Header h;
  h.type = PacketType::Ack;
  h.session_id = 0xDEADBEEF;
  h.seq = 0x11223344;
  h.ack = 0x55667788;
  h.window = 4096;
  SackBlock s[3] = {{100, 105}, {200, 201}, {4000000000u, 4000000005u}};
  std::vector<std::uint8_t> pay(37);
  for (std::size_t i = 0; i < pay.size(); ++i) pay[i] = static_cast<std::uint8_t>(i * 7);

  auto buf = build(h, s, 3, pay.data(), pay.size());
  CHECK_EQ(buf.size(), kHeaderSize + 3 * kSackBlockSize + pay.size());

  Parsed p = parse(buf);
  CHECK_EQ(static_cast<int>(p.r), static_cast<int>(ParseResult::Ok));
  CHECK_EQ(p.h.session_id, 0xDEADBEEFu);
  CHECK_EQ(p.h.seq, 0x11223344u);
  CHECK_EQ(p.h.ack, 0x55667788u);
  CHECK_EQ(p.h.window, 4096);
  CHECK_EQ(p.h.payload_len, static_cast<std::uint16_t>(pay.size()));
  CHECK_EQ(p.nsack, static_cast<std::size_t>(3));
  for (int i = 0; i < 3; ++i) {
    CHECK_EQ(p.sacks[i].start, s[i].start);
    CHECK_EQ(p.sacks[i].end, s[i].end);
  }
  CHECK(std::memcmp(p.payload, pay.data(), pay.size()) == 0);
}

TEST(packet, "wire format is big-endian and 24 bytes of header") {
  Header h;
  h.type = PacketType::Data;
  h.session_id = 0x01020304;
  h.seq = 0x05060708;
  h.ack = 0x090A0B0C;
  h.window = 0x0D0E;
  auto buf = build(h, nullptr, 0, nullptr, 0);
  CHECK_EQ(buf.size(), kHeaderSize);
  CHECK_EQ(buf[0], kVersion);
  CHECK_EQ(buf[1], static_cast<std::uint8_t>(PacketType::Data));
  CHECK_EQ(buf[2], 0);
  CHECK_EQ(buf[3], 0);
  const std::uint8_t want[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                               0x09, 0x0A, 0x0B, 0x0C, 0x00, 0x00, 0x0D, 0x0E};
  CHECK(std::memcmp(buf.data() + 4, want, sizeof(want)) == 0);
}

TEST(packet, "every single-bit flip is detected") {
  Header h;
  h.type = PacketType::Data;
  h.session_id = 0x52554450;
  h.seq = 12345;
  std::vector<std::uint8_t> pay(200);
  for (std::size_t i = 0; i < pay.size(); ++i) pay[i] = static_cast<std::uint8_t>(i);
  const auto good = build(h, nullptr, 0, pay.data(), pay.size());
  CHECK_EQ(static_cast<int>(parse(good).r), static_cast<int>(ParseResult::Ok));

  std::size_t detected = 0, total = 0;
  for (std::size_t byte = 0; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto bad = good;
      poke(bad, byte, static_cast<std::uint8_t>(bad[byte] ^ (1u << bit)));
      ++total;
      if (parse(bad).r != ParseResult::Ok) ++detected;
    }
  }
  CHECK_EQ(detected, total);  // CRC-32C detects all 1-bit errors by construction
}

TEST(packet, "truncated and over-long datagrams are rejected") {
  Header h;
  h.type = PacketType::Data;
  std::vector<std::uint8_t> pay(64, 0xAB);
  const auto good = build(h, nullptr, 0, pay.data(), pay.size());
  for (std::size_t n = 0; n < good.size(); ++n) {
    std::vector<std::uint8_t> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
    Parsed p;
    p.r = parse_packet(t.data(), t.size(), p.h, p.sacks, p.nsack, p.payload, p.plen);
    CHECK(p.r != ParseResult::Ok);
  }
  auto longer = good;
  longer.push_back(0);
  CHECK_EQ(static_cast<int>(parse(longer).r), static_cast<int>(ParseResult::LengthMismatch));
}

TEST(packet, "bad version, type, reserved and sack count are rejected") {
  Header h;
  h.type = PacketType::Ack;
  auto buf = build(h, nullptr, 0, nullptr, 0);

  auto v = buf; poke(v, 0, 99);
  CHECK_EQ(static_cast<int>(parse(v).r), static_cast<int>(ParseResult::BadVersion));
  auto t = buf; poke(t, 1, 0);
  CHECK_EQ(static_cast<int>(parse(t).r), static_cast<int>(ParseResult::BadType));
  poke(t, 1, 200);
  CHECK_EQ(static_cast<int>(parse(t).r), static_cast<int>(ParseResult::BadType));
  auto r = buf; poke(r, 3, 1);
  CHECK_EQ(static_cast<int>(parse(r).r), static_cast<int>(ParseResult::BadReserved));
  auto s = buf; poke(s, 2, 200);
  CHECK_EQ(static_cast<int>(parse(s).r), static_cast<int>(ParseResult::BadSackCount));
}

TEST(packet, "declared payload length inconsistent with datagram size is rejected") {
  Header h;
  h.type = PacketType::Data;
  std::vector<std::uint8_t> pay(10, 1);
  auto buf = build(h, nullptr, 0, pay.data(), pay.size());
  // Claim a huge payload without resizing.
  poke(buf, 16, 0xFF);
  poke(buf, 17, 0xFF);
  CHECK(parse(buf).r != ParseResult::Ok);
}

TEST(packet, "random garbage never parses as valid and never reads out of bounds") {
  std::mt19937_64 rng(20260826);
  std::size_t accepted = 0;
  for (int i = 0; i < 200000; ++i) {
    std::uniform_int_distribution<std::size_t> ld(0, 80);
    std::vector<std::uint8_t> buf(ld(rng));
    for (auto& b : buf) b = static_cast<std::uint8_t>(rng() & 0xFF);
    Parsed p;
    p.r = parse_packet(buf.data(), buf.size(), p.h, p.sacks, p.nsack, p.payload, p.plen);
    if (p.r == ParseResult::Ok) ++accepted;
  }
  // A random 32-bit CRC match is ~2^-32 per attempt; 200k attempts must not hit.
  CHECK_EQ(accepted, static_cast<std::size_t>(0));
}
