#include "reliable_udp/packet.hpp"

#include <cstring>

#include "reliable_udp/crc32c.hpp"

namespace rudp {
namespace {

inline void put_u16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}
inline void put_u32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}
inline std::uint16_t get_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
inline std::uint32_t get_u32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

}  // namespace

const char* packet_type_name(PacketType t) {
  switch (t) {
    case PacketType::Data:   return "DATA";
    case PacketType::Ack:    return "ACK";
    case PacketType::Syn:    return "SYN";
    case PacketType::SynAck: return "SYN_ACK";
    case PacketType::Fin:    return "FIN";
    case PacketType::FinAck: return "FIN_ACK";
    case PacketType::Rst:    return "RST";
  }
  return "?";
}

bool is_valid_type(std::uint8_t t) {
  return t >= static_cast<std::uint8_t>(PacketType::Data) &&
         t <= static_cast<std::uint8_t>(PacketType::Rst);
}

const char* parse_result_name(ParseResult r) {
  switch (r) {
    case ParseResult::Ok:             return "ok";
    case ParseResult::TooShort:       return "too_short";
    case ParseResult::BadVersion:     return "bad_version";
    case ParseResult::BadType:        return "bad_type";
    case ParseResult::BadReserved:    return "bad_reserved";
    case ParseResult::BadSackCount:   return "bad_sack_count";
    case ParseResult::LengthMismatch: return "length_mismatch";
    case ParseResult::BadChecksum:    return "bad_checksum";
  }
  return "?";
}

std::size_t encode_packet(std::uint8_t* out, const Header& h, const SackBlock* sacks,
                          std::size_t sack_count, const std::uint8_t* payload,
                          std::size_t payload_len) {
  if (sack_count > kMaxSackBlocks) sack_count = kMaxSackBlocks;

  out[0] = h.version;
  out[1] = static_cast<std::uint8_t>(h.type);
  out[2] = static_cast<std::uint8_t>(sack_count);
  out[3] = 0;
  put_u32(out + 4, h.session_id);
  put_u32(out + 8, h.seq);
  put_u32(out + 12, h.ack);
  put_u16(out + 16, static_cast<std::uint16_t>(payload_len));
  put_u16(out + 18, h.window);
  put_u32(out + kChecksumOffset, 0);

  std::size_t off = kHeaderSize;
  for (std::size_t i = 0; i < sack_count; ++i) {
    put_u32(out + off, sacks[i].start);
    put_u32(out + off + 4, sacks[i].end);
    off += kSackBlockSize;
  }

  std::uint32_t st = crc32c_init();
  st = crc32c_update(st, out, off);
  if (payload_len > 0) st = crc32c_update(st, payload, payload_len);
  put_u32(out + kChecksumOffset, crc32c_final(st));
  return off;
}

ParseResult parse_packet(const std::uint8_t* buf, std::size_t len, Header& h,
                         SackBlock* sacks_out, std::size_t& sack_count_out,
                         const std::uint8_t*& payload, std::size_t& payload_len) {
  payload = nullptr;
  payload_len = 0;
  sack_count_out = 0;

  if (len < kHeaderSize) return ParseResult::TooShort;

  h.version = buf[0];
  if (h.version != kVersion) return ParseResult::BadVersion;
  if (!is_valid_type(buf[1])) return ParseResult::BadType;
  h.type = static_cast<PacketType>(buf[1]);
  h.sack_count = buf[2];
  h.reserved = buf[3];
  if (h.reserved != 0) return ParseResult::BadReserved;
  if (h.sack_count > kMaxSackBlocks) return ParseResult::BadSackCount;

  h.session_id  = get_u32(buf + 4);
  h.seq         = get_u32(buf + 8);
  h.ack         = get_u32(buf + 12);
  h.payload_len = get_u16(buf + 16);
  h.window      = get_u16(buf + 18);
  h.checksum    = get_u32(buf + kChecksumOffset);

  const std::size_t sack_bytes = static_cast<std::size_t>(h.sack_count) * kSackBlockSize;
  const std::size_t expected = kHeaderSize + sack_bytes + h.payload_len;
  // Exact-length check: a truncated or over-long datagram is rejected outright
  // rather than silently reinterpreted.
  if (expected != len) return ParseResult::LengthMismatch;

  // Checksum with the checksum field treated as zero.
  static const std::uint8_t kZero4[4] = {0, 0, 0, 0};
  std::uint32_t st = crc32c_init();
  st = crc32c_update(st, buf, kChecksumOffset);
  st = crc32c_update(st, kZero4, 4);
  st = crc32c_update(st, buf + kChecksumOffset + 4, len - kChecksumOffset - 4);
  if (crc32c_final(st) != h.checksum) return ParseResult::BadChecksum;

  std::size_t off = kHeaderSize;
  for (std::size_t i = 0; i < h.sack_count; ++i) {
    sacks_out[i].start = get_u32(buf + off);
    sacks_out[i].end   = get_u32(buf + off + 4);
    off += kSackBlockSize;
  }
  sack_count_out = h.sack_count;

  if (h.payload_len > 0) {
    payload = buf + off;
    payload_len = h.payload_len;
  }
  return ParseResult::Ok;
}

}  // namespace rudp
