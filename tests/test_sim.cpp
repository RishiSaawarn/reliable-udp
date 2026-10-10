// The simulator is test infrastructure, so it gets tested too: if its loss
// rate or determinism were wrong, every reliability number would be wrong.
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "reliable_udp/sim/net_sim.hpp"
#include "reliable_udp/sim/sim_harness.hpp"
#include "test_framework.hpp"

using namespace rudp;
using namespace rudp::sim;

namespace {

struct Driver {
  std::uint64_t now = 0;
  std::mt19937_64 rng;
  SimLink link;
  explicit Driver(const LinkConfig& c, std::uint64_t seed)
      : rng(seed), link("t", c, rng) {
    link.bind_clock(&now);
  }
  void send(std::size_t n) {
    std::vector<std::uint8_t> buf(n, 0xAA);
    link.send_datagram(buf.data(), buf.size(), nullptr, 0);
  }
};

}  // namespace

TEST(sim, "loss probability is honoured within statistical tolerance") {
  for (double p : {0.0, 0.01, 0.05, 0.10, 0.20, 0.50}) {
    LinkConfig c;
    c.loss_prob = p;
    Driver d(c, 12345);
    const int N = 200000;
    for (int i = 0; i < N; ++i) d.send(100);
    const double observed =
        static_cast<double>(d.link.stats().dropped_loss) / static_cast<double>(N);
    // 4 sigma of a binomial with n = 200000
    const double sigma = std::sqrt(p * (1.0 - p) / N);
    CHECK_MSG(std::fabs(observed - p) <= 4.0 * sigma + 1e-9,
              "loss " + std::to_string(p) + " observed " + std::to_string(observed));
  }
}

TEST(sim, "duplication and reordering probabilities are honoured") {
  LinkConfig c;
  c.dup_prob = 0.10;
  c.reorder_prob = 0.25;
  c.reorder_extra_us = 1000;
  Driver d(c, 999);
  const int N = 100000;
  for (int i = 0; i < N; ++i) d.send(100);
  const double dup = static_cast<double>(d.link.stats().duplicated) / N;
  const double reo = static_cast<double>(d.link.stats().reordered) / N;
  CHECK(std::fabs(dup - 0.10) < 0.005);
  CHECK(std::fabs(reo - 0.25) < 0.006);
  CHECK_EQ(d.link.stats().offered, static_cast<std::uint64_t>(N));
}

TEST(sim, "the same seed produces an identical event stream") {
  LinkConfig c;
  c.loss_prob = 0.1;
  c.dup_prob = 0.1;
  c.reorder_prob = 0.2;
  c.corrupt_prob = 0.05;
  c.delay_us = 1000;
  c.jitter_us = 300;
  c.reorder_extra_us = 2000;

  auto run = [&](std::uint64_t seed) {
    Driver d(c, seed);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> trace;
    std::vector<SimDatagram> out;
    for (int i = 0; i < 5000; ++i) {
      d.send(200);
      d.now += 100;
      out.clear();
      d.link.drain_due(d.now, out);
      for (const auto& p : out) trace.emplace_back(p.deliver_us, p.send_order);
    }
    d.now += 10000000;
    out.clear();
    d.link.drain_due(d.now, out);
    for (const auto& p : out) trace.emplace_back(p.deliver_us, p.send_order);
    return trace;
  };
  const auto a = run(4242);
  const auto b = run(4242);
  const auto c2 = run(4243);
  CHECK(a == b);
  CHECK(a != c2);
  CHECK(!a.empty());
}

TEST(sim, "delay and jitter land in the configured band") {
  LinkConfig c;
  c.delay_us = 10000;
  c.jitter_us = 2000;
  Driver d(c, 77);
  for (int i = 0; i < 5000; ++i) d.send(100);
  std::vector<SimDatagram> out;
  d.link.drain_due(1000000, out);
  CHECK_EQ(out.size(), static_cast<std::size_t>(5000));
  std::uint64_t lo = UINT64_MAX, hi = 0, sum = 0;
  for (const auto& p : out) {
    lo = std::min(lo, p.deliver_us);
    hi = std::max(hi, p.deliver_us);
    sum += p.deliver_us;
  }
  CHECK(lo >= 8000);
  CHECK(hi <= 12000);
  const double mean = static_cast<double>(sum) / 5000.0;
  CHECK(std::fabs(mean - 10000.0) < 100.0);
}

TEST(sim, "rate limiting serialises packets onto the link") {
  LinkConfig c;
  c.rate_bps = 8000000;  // 1 MB/s -> 1000 us per 1000-byte datagram
  Driver d(c, 5);
  for (int i = 0; i < 10; ++i) d.send(1000);
  std::vector<SimDatagram> out;
  d.link.drain_due(1000000, out);
  CHECK_EQ(out.size(), static_cast<std::size_t>(10));
  for (std::size_t i = 0; i < out.size(); ++i) {
    CHECK_EQ(out[i].deliver_us, static_cast<std::uint64_t>(i + 1) * 1000);
  }
}

TEST(sim, "a drop-tail queue limit drops the overflow") {
  LinkConfig c;
  c.queue_limit_bytes = 5000;
  c.delay_us = 100000;
  Driver d(c, 5);
  for (int i = 0; i < 20; ++i) d.send(1000);
  CHECK_EQ(d.link.stats().dropped_queue, 15u);
  CHECK_EQ(d.link.stats().offered, 20u);
}

TEST(sim, "reordering actually delivers packets out of transmit order") {
  LinkConfig c;
  c.delay_us = 1000;
  c.reorder_prob = 0.5;
  c.reorder_extra_us = 5000;
  Driver d(c, 31337);
  for (int i = 0; i < 2000; ++i) {
    d.send(100);
    d.now += 100;
  }
  std::vector<SimDatagram> out;
  d.link.drain_due(d.now + 100000, out);
  CHECK(d.link.stats().out_of_order_deliveries > 100);
}

TEST(sim, "a scripted fault overrides the probabilistic model") {
  LinkConfig c;
  c.loss_prob = 1.0;  // would drop everything
  Driver d(c, 1);
  d.link.set_script([](std::uint64_t idx, const std::uint8_t*, std::size_t,
                       FaultDecision& fd) {
    if (idx % 2 == 0) return false;  // fall through to the 100% loss model
    fd.drop = false;
    return true;                      // survive
  });
  for (int i = 0; i < 100; ++i) d.send(50);
  CHECK_EQ(d.link.stats().dropped_loss, 50u);
  CHECK_EQ(d.link.stats().dropped_script, 0u);
  std::vector<SimDatagram> out;
  d.link.drain_due(1000000, out);
  CHECK_EQ(out.size(), static_cast<std::size_t>(50));
}

TEST(sim, "corruption flips exactly one bit") {
  LinkConfig c;
  c.corrupt_prob = 1.0;
  Driver d(c, 8);
  std::vector<std::uint8_t> ref(200, 0x33);
  for (int i = 0; i < 500; ++i) d.link.send_datagram(ref.data(), ref.size(), nullptr, 0);
  std::vector<SimDatagram> out;
  d.link.drain_due(1000000, out);
  CHECK_EQ(out.size(), static_cast<std::size_t>(500));
  for (const auto& p : out) {
    CHECK_EQ(p.bytes.size(), ref.size());
    int diff_bits = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) {
      diff_bits += __builtin_popcount(static_cast<unsigned>(p.bytes[i] ^ ref[i]));
    }
    CHECK_EQ(diff_bits, 1);
  }
}
