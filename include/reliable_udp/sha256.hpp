#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rudp {

class Sha256 {
 public:
  Sha256();
  void update(const std::uint8_t* data, std::size_t len);
  std::array<std::uint8_t, 32> finish();

 private:
  void compress(const std::uint8_t* block);
  std::uint32_t h_[8];
  std::uint8_t buf_[64];
  std::size_t buf_len_ = 0;
  std::uint64_t total_bits_ = 0;
};

std::string sha256_hex(const std::uint8_t* data, std::size_t len);
std::string to_hex(const std::array<std::uint8_t, 32>& d);

}  // namespace rudp
