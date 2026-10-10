#pragma once
#include <cstddef>
#include <cstdint>

namespace rudp {

// The only way the protocol engines touch the outside world.
//
// The interface is a gather-write on purpose: the header lives in a small
// stack buffer owned by the caller and the payload points straight into the
// application's source buffer, so the real-socket implementation can issue a
// two-iovec sendmsg() with no copy and no allocation.
class DatagramSink {
 public:
  virtual ~DatagramSink() = default;
  virtual void send_datagram(const std::uint8_t* head, std::size_t head_len,
                             const std::uint8_t* payload, std::size_t payload_len) = 0;
};

inline constexpr std::size_t kMaxDatagramSize = 65535;

}  // namespace rudp
