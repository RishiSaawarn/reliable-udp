// Wire format. See docs/PROTOCOL.md for the normative description.
#pragma once
#include <cstddef>
#include <cstdint>

#include "reliable_udp/seq.hpp"

namespace rudp {

inline constexpr std::uint8_t kVersion = 1;

// Fixed header, big-endian (network byte order):
//   0  1  version
//   1  1  type
//   2  1  sack_count      number of 8-byte SACK blocks following the header
//   3  1  reserved        must be 0 (explicit padding: keeps seq/ack 4-aligned)
//   4  4  session_id
//   8  4  seq
//  12  4  ack             cumulative ACK = next sequence number expected
//  16  2  payload_len
//  18  2  window          receiver's advertised free buffer, in packets
//  20  4  checksum        CRC-32C over the whole datagram, this field zeroed
inline constexpr std::size_t kHeaderSize     = 24;
inline constexpr std::size_t kChecksumOffset = 20;
inline constexpr std::size_t kMaxSackBlocks  = 8;
inline constexpr std::size_t kSackBlockSize  = 8;
inline constexpr std::size_t kMaxOverhead    = kHeaderSize + kMaxSackBlocks * kSackBlockSize;

enum class PacketType : std::uint8_t {
  Data   = 1,
  Ack    = 2,
  Syn    = 3,
  SynAck = 4,
  Fin    = 5,
  FinAck = 6,
  Rst    = 7,
};

const char* packet_type_name(PacketType t);
bool is_valid_type(std::uint8_t t);

struct SackBlock {
  seq_t start = 0;  // inclusive
  seq_t end   = 0;  // exclusive
};

struct Header {
  std::uint8_t  version    = kVersion;
  PacketType    type       = PacketType::Data;
  std::uint8_t  sack_count = 0;
  std::uint8_t  reserved   = 0;
  std::uint32_t session_id = 0;
  seq_t         seq        = 0;
  seq_t         ack        = 0;
  std::uint16_t payload_len = 0;
  std::uint16_t window      = 0;
  std::uint32_t checksum    = 0;
};

enum class ParseResult {
  Ok = 0,
  TooShort,
  BadVersion,
  BadType,
  BadReserved,
  BadSackCount,
  LengthMismatch,
  BadChecksum,
};

const char* parse_result_name(ParseResult r);

// Serialises header + SACK blocks into `out` (which must hold at least
// kHeaderSize + sack_count * kSackBlockSize bytes) and computes the checksum
// over that prefix followed by `payload`. The payload itself is never copied,
// which lets the sender hand the file buffer straight to sendmsg().
// Returns the number of bytes written to `out`.
std::size_t encode_packet(std::uint8_t* out, const Header& h, const SackBlock* sacks,
                          std::size_t sack_count, const std::uint8_t* payload,
                          std::size_t payload_len);

// Parses and fully validates a received datagram, checksum included.
// On Ok, `payload` points into `buf` and `payload_len` is set.
// `sacks_out` must have room for kMaxSackBlocks entries.
ParseResult parse_packet(const std::uint8_t* buf, std::size_t len, Header& h,
                         SackBlock* sacks_out, std::size_t& sack_count_out,
                         const std::uint8_t*& payload, std::size_t& payload_len);

}  // namespace rudp
