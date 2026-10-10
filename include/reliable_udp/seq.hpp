// Serial (wrap-around) sequence-number arithmetic, RFC 1982 style.
#pragma once
#include <cstdint>

namespace rudp {

using seq_t = std::uint32_t;

// Signed distance a - b in the circular 32-bit sequence space.
// Valid only when the true distance is < 2^31, which the protocol guarantees
// because the send and receive windows are bounded far below that.
inline std::int32_t seq_diff(seq_t a, seq_t b) noexcept {
  return static_cast<std::int32_t>(a - b);
}
inline bool seq_lt(seq_t a, seq_t b) noexcept { return seq_diff(a, b) < 0; }
inline bool seq_le(seq_t a, seq_t b) noexcept { return seq_diff(a, b) <= 0; }
inline bool seq_gt(seq_t a, seq_t b) noexcept { return seq_diff(a, b) > 0; }
inline bool seq_ge(seq_t a, seq_t b) noexcept { return seq_diff(a, b) >= 0; }

// True when lo <= s < hi in serial arithmetic (hi - lo must be < 2^31).
inline bool seq_in_range(seq_t s, seq_t lo, seq_t hi) noexcept {
  return seq_ge(s, lo) && seq_lt(s, hi);
}

}  // namespace rudp
