// Proves the claim "no heap allocation in the steady-state packet path".
//
// Global operator new/delete are replaced with counting versions. The counter
// is armed only after every buffer has been constructed, then a complete
// windowed exchange is driven by hand -- sender -> receiver -> ACK -> sender --
// with fixed, pre-sized relay buffers. Any allocation from inside the engines
// during that phase fails the test.
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "reliable_udp/receiver.hpp"
#include "reliable_udp/sender.hpp"
#include "test_framework.hpp"

namespace alloc_probe {

std::atomic<bool> armed{false};
std::atomic<std::size_t> allocations{0};
std::atomic<std::size_t> deallocations{0};

inline void arm() {
  allocations.store(0, std::memory_order_relaxed);
  deallocations.store(0, std::memory_order_relaxed);
  armed.store(true, std::memory_order_relaxed);
}
inline void disarm() { armed.store(false, std::memory_order_relaxed); }

}  // namespace alloc_probe

// GCC cannot see that these replacements are a matched malloc/free pair,
// because the allocation and deallocation sides are separate functions.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

void* operator new(std::size_t n) {
  if (alloc_probe::armed.load(std::memory_order_relaxed)) {
    alloc_probe::allocations.fetch_add(1, std::memory_order_relaxed);
  }
  if (n == 0) n = 1;
  void* p = std::malloc(n);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
  if (p && alloc_probe::armed.load(std::memory_order_relaxed)) {
    alloc_probe::deallocations.fetch_add(1, std::memory_order_relaxed);
  }
  std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

#pragma GCC diagnostic pop

using namespace rudp;

namespace {

// A fixed-capacity relay: datagrams are copied into preallocated slots, so the
// relay itself cannot allocate while the probe is armed.
class RelayQueue final : public DatagramSink {
 public:
  static constexpr std::size_t kSlots = 4096;
  static constexpr std::size_t kSlotBytes = 2048;

  RelayQueue() : storage_(kSlots * kSlotBytes), len_(kSlots, 0) {}

  void send_datagram(const std::uint8_t* head, std::size_t hl, const std::uint8_t* pay,
                     std::size_t pl) override {
    if (count_ >= kSlots) {
      ++overflow_;
      return;
    }
    const std::size_t idx = (head_ + count_) % kSlots;
    std::uint8_t* dst = storage_.data() + idx * kSlotBytes;
    std::memcpy(dst, head, hl);
    if (pl) std::memcpy(dst + hl, pay, pl);
    len_[idx] = hl + pl;
    ++count_;
  }

  bool pop(const std::uint8_t*& p, std::size_t& n) {
    if (count_ == 0) return false;
    p = storage_.data() + head_ * kSlotBytes;
    n = len_[head_];
    head_ = (head_ + 1) % kSlots;
    --count_;
    return true;
  }
  std::size_t size() const { return count_; }
  std::size_t overflow() const { return overflow_; }

 private:
  std::vector<std::uint8_t> storage_;
  std::vector<std::size_t> len_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  std::size_t overflow_ = 0;
};

}  // namespace

TEST(alloc, "the probe itself works") {
  // Direct calls to the replaced operators, which the optimiser is not allowed
  // to elide the way it may elide a matched new/delete expression pair.
  alloc_probe::arm();
  void* volatile p = ::operator new(64);
  ::operator delete(const_cast<void*>(p));
  alloc_probe::disarm();
  CHECK(alloc_probe::allocations.load() >= 1);
  CHECK(alloc_probe::deallocations.load() >= 1);
}

TEST(alloc, "a full windowed transfer performs zero heap allocations") {
  Config c;
  c.max_payload = 1200;
  c.send_window = 64;
  c.recv_window = 128;
  c.min_rto_us = 1000;
  c.linger_us = 10000;
  c.normalize();

  // ---- setup phase: allocation is expected and allowed --------------------
  std::vector<std::uint8_t> data(2 * 1024 * 1024, 0);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 31);
  std::vector<std::uint8_t> received;
  received.reserve(data.size() + 4096);

  RelayQueue to_receiver;
  RelayQueue to_sender;

  std::size_t got = 0;
  SrReceiver receiver(c, to_sender, [&](const std::uint8_t* p, std::size_t n) {
    // Writing through a reserved vector: no reallocation, no new.
    std::memcpy(received.data() + got, p, n);
    got += n;
  });
  received.resize(data.size() + 4096);
  SrSender sender(c, to_receiver, data.data(), data.size());

  std::uint64_t now = 0;
  sender.start(now);
  // Warm up one exchange so any lazily-initialised state is already there.
  {
    const std::uint8_t* p;
    std::size_t n;
    while (to_receiver.pop(p, n)) receiver.on_datagram(p, n, now);
    while (to_sender.pop(p, n)) sender.on_datagram(p, n, now);
  }

  // ---- measured phase -----------------------------------------------------
  alloc_probe::arm();
  std::uint64_t iterations = 0;
  while (!(sender.done() && receiver.done()) && iterations < 5000000) {
    ++iterations;
    const std::uint8_t* p;
    std::size_t n;
    bool progress = false;
    while (to_receiver.pop(p, n)) {
      receiver.on_datagram(p, n, now);
      progress = true;
    }
    while (to_sender.pop(p, n)) {
      sender.on_datagram(p, n, now);
      progress = true;
    }
    if (!progress) now += 1000;
    sender.tick(now);
    receiver.tick(now);
    if (sender.failed()) break;
  }
  alloc_probe::disarm();

  const std::size_t allocs = alloc_probe::allocations.load();
  CHECK_MSG(allocs == 0, "steady-state heap allocations: " + std::to_string(allocs) +
                             " over " + std::to_string(iterations) + " loop iterations");
  CHECK_MSG(!sender.failed(), std::string("sender failed: ") + sender.failure_reason());
  CHECK(sender.done());
  CHECK(receiver.done());
  CHECK_EQ(got, data.size());
  CHECK(std::memcmp(received.data(), data.data(), data.size()) == 0);
  CHECK_EQ(to_receiver.overflow(), static_cast<std::size_t>(0));
  CHECK_EQ(to_sender.overflow(), static_cast<std::size_t>(0));
  // Each iteration drains a full window, so a 2 MiB / 64-packet transfer takes
  // on the order of 1748/64 = 28 rounds.
  CHECK(iterations > 20);
  CHECK(sender.stats().data_packets_sent >= 1748u);
}

TEST(alloc, "loss and reordering on the hot path still allocate nothing") {
  Config c;
  c.max_payload = 1000;
  c.send_window = 32;
  c.recv_window = 64;
  c.min_rto_us = 1000;
  c.linger_us = 10000;
  c.normalize();

  std::vector<std::uint8_t> data(512 * 1024, 0x7E);
  std::vector<std::uint8_t> received(data.size() + 4096);
  RelayQueue to_receiver, to_sender;
  std::size_t got = 0;
  SrReceiver receiver(c, to_sender, [&](const std::uint8_t* p, std::size_t n) {
    std::memcpy(received.data() + got, p, n);
    got += n;
  });
  SrSender sender(c, to_receiver, data.data(), data.size());

  std::uint64_t now = 0;
  std::uint64_t rngstate = 12345;
  auto rnd = [&]() {
    rngstate = rngstate * 6364136223846793005ull + 1442695040888963407ull;
    return (rngstate >> 33) % 100;
  };
  sender.start(now);

  alloc_probe::arm();
  std::uint64_t iterations = 0;
  while (!(sender.done() && receiver.done()) && iterations < 5000000) {
    ++iterations;
    const std::uint8_t* p;
    std::size_t n;
    bool progress = false;
    while (to_receiver.pop(p, n)) {
      if (rnd() >= 10) receiver.on_datagram(p, n, now);  // 10% forward loss
      progress = true;
    }
    while (to_sender.pop(p, n)) {
      if (rnd() >= 5) sender.on_datagram(p, n, now);     // 5% ACK loss
      progress = true;
    }
    if (!progress) now += 500;
    sender.tick(now);
    receiver.tick(now);
    if (sender.failed()) break;
  }
  alloc_probe::disarm();

  CHECK_MSG(!sender.failed(), std::string("sender failed: ") + sender.failure_reason());
  CHECK_EQ(got, data.size());
  CHECK(sender.stats().retransmits > 0);
  CHECK_MSG(alloc_probe::allocations.load() == 0,
            "steady-state heap allocations under loss: " +
                std::to_string(alloc_probe::allocations.load()));
}
