#include <cmath>

#include "reliable_udp/config.hpp"
#include "reliable_udp/rtt.hpp"
#include "reliable_udp/seq.hpp"
#include "test_framework.hpp"

using namespace rudp;

TEST(seq, "serial arithmetic is correct across the 32-bit wrap point") {
  CHECK(seq_lt(1u, 2u));
  CHECK(seq_gt(2u, 1u));
  CHECK(seq_lt(0xFFFFFFFFu, 0u));         // wraps forward
  CHECK(seq_lt(0xFFFFFF00u, 0x00000010u));
  CHECK(seq_gt(0x00000010u, 0xFFFFFF00u));
  CHECK_EQ(seq_diff(0x00000010u, 0xFFFFFF00u), 0x110);
  CHECK_EQ(seq_diff(0xFFFFFF00u, 0x00000010u), -0x110);
  CHECK(seq_le(7u, 7u));
  CHECK(seq_ge(7u, 7u));
}

TEST(seq, "range test wraps") {
  CHECK(seq_in_range(0xFFFFFFF0u, 0xFFFFFF00u, 0x00000100u));
  CHECK(seq_in_range(0x00000050u, 0xFFFFFF00u, 0x00000100u));
  CHECK(!seq_in_range(0x00000100u, 0xFFFFFF00u, 0x00000100u));  // end exclusive
  CHECK(!seq_in_range(0xFFFFFEFFu, 0xFFFFFF00u, 0x00000100u));
}

TEST(seq, "config normalises windows to powers of two") {
  Config c;
  c.send_window = 100;
  c.recv_window = 3;
  c.normalize();
  CHECK_EQ(c.send_window, 128u);
  CHECK_EQ(c.recv_window, 4u);
  CHECK_EQ(round_up_pow2(1), 1u);
  CHECK_EQ(round_up_pow2(64), 64u);
  CHECK_EQ(round_up_pow2(65), 128u);
}

TEST(rtt, "first sample seeds srtt and rttvar per RFC 6298 2.2") {
  Config c;
  c.min_rto_us = 1;
  c.max_rto_us = 10000000;
  c.clock_granularity_us = 1;
  c.normalize();
  RttEstimator e(c);
  CHECK_EQ(e.rto_us(), c.init_rto_us);
  e.on_sample(100000);
  CHECK(std::fabs(e.srtt_us() - 100000.0) < 1e-6);
  CHECK(std::fabs(e.rttvar_us() - 50000.0) < 1e-6);
  // RTO = SRTT + max(G, 4*RTTVAR) = 100000 + 200000
  CHECK_EQ(e.rto_us(), 300000u);
}

TEST(rtt, "subsequent samples apply the 1/8 and 1/4 gains") {
  Config c;
  c.min_rto_us = 1;
  c.max_rto_us = 100000000;
  c.clock_granularity_us = 1;
  c.normalize();
  RttEstimator e(c);
  e.on_sample(100000);
  e.on_sample(200000);
  // rttvar = 0.75*50000 + 0.25*|100000-200000| = 37500+25000 = 62500
  // srtt   = 0.875*100000 + 0.125*200000 = 112500
  CHECK(std::fabs(e.rttvar_us() - 62500.0) < 1e-6);
  CHECK(std::fabs(e.srtt_us() - 112500.0) < 1e-6);
  CHECK_EQ(e.rto_us(), static_cast<std::uint64_t>(112500 + 4 * 62500));
}

TEST(rtt, "a stable link converges to a tight RTO") {
  Config c;
  c.min_rto_us = 1;
  c.max_rto_us = 100000000;
  c.clock_granularity_us = 1;
  c.normalize();
  RttEstimator e(c);
  for (int i = 0; i < 200; ++i) e.on_sample(50000);
  CHECK(std::fabs(e.srtt_us() - 50000.0) < 100.0);
  CHECK(e.rttvar_us() < 100.0);
  CHECK(e.rto_us() < 55000u);
  CHECK_EQ(e.samples(), 200u);
  CHECK(std::fabs(e.mean_rtt_us() - 50000.0) < 1e-6);
}

TEST(rtt, "timeouts back the RTO off exponentially and a sample restores it") {
  Config c;
  c.min_rto_us = 1000;
  c.max_rto_us = 100000000;
  c.clock_granularity_us = 1;
  c.normalize();
  RttEstimator e(c);
  e.on_sample(10000);  // srtt 10000, rttvar 5000 -> rto 30000
  CHECK_EQ(e.rto_us(), 30000u);
  e.on_timeout();
  CHECK_EQ(e.rto_us(), 60000u);
  e.on_timeout();
  CHECK_EQ(e.rto_us(), 120000u);
  CHECK_EQ(e.backoff_shift(), 2u);
  e.on_sample(10000);
  CHECK_EQ(e.backoff_shift(), 0u);
  CHECK(e.rto_us() < 40000u);
  CHECK_EQ(e.timeouts(), 2u);
}

TEST(rtt, "the RTO is clamped to the configured floor and ceiling") {
  Config c;
  c.min_rto_us = 20000;
  c.max_rto_us = 200000;
  c.normalize();
  RttEstimator e(c);
  e.on_sample(10);  // would give ~25us
  CHECK_EQ(e.rto_us(), 20000u);
  RttEstimator e2(c);
  e2.on_sample(5000000);
  CHECK_EQ(e2.rto_us(), 200000u);
  for (int i = 0; i < 20; ++i) e2.on_timeout();
  CHECK_EQ(e2.rto_us(), 200000u);  // backoff cannot exceed the ceiling
}
