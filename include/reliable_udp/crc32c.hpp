// CRC-32C (Castagnoli, polynomial 0x1EDC6F41, reflected 0x82F63B78).
// Software table implementation; the table is built at compile time.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace rudp {
namespace detail {
constexpr std::array<std::uint32_t, 256> make_crc32c_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}
inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();
}  // namespace detail

inline std::uint32_t crc32c_init() noexcept { return 0xFFFFFFFFu; }

inline std::uint32_t crc32c_update(std::uint32_t state, const std::uint8_t* p,
                                   std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    state = detail::kCrc32cTable[(state ^ p[i]) & 0xFFu] ^ (state >> 8);
  }
  return state;
}

inline std::uint32_t crc32c_final(std::uint32_t state) noexcept { return ~state; }

inline std::uint32_t crc32c(const std::uint8_t* p, std::size_t n) noexcept {
  return crc32c_final(crc32c_update(crc32c_init(), p, n));
}

}  // namespace rudp
