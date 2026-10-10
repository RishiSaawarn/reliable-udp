// Real POSIX UDP sockets on loopback. Same engines as the simulator; what is
// under test here is the socket plumbing, the timing loop, and the threaded
// multi-session server.
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "reliable_udp/net/server.hpp"
#include "reliable_udp/net/session_runner.hpp"
#include "reliable_udp/sha256.hpp"
#include "reliable_udp/sim/sim_harness.hpp"
#include "test_framework.hpp"

using namespace rudp;
using namespace rudp::net;

namespace {

std::vector<std::uint8_t> make_data(std::size_t n, std::uint64_t seed) {
  std::vector<std::uint8_t> v(n);
  rudp::sim::fill_pattern(v, seed);
  return v;
}

RunnerConfig runner_config(std::uint32_t window = 64) {
  RunnerConfig rc;
  rc.proto.send_window = window;
  rc.proto.recv_window = window * 2;
  rc.proto.max_payload = 1200;
  rc.proto.min_rto_us = 2000;
  rc.proto.init_rto_us = 50000;
  rc.proto.linger_us = 50000;
  rc.proto.normalize();
  rc.idle_timeout_us = 20ull * 1000 * 1000;
  return rc;
}

// Deterministic drop filter for the real-socket path.
struct LossFilter {
  std::mt19937_64 rng;
  double p;
  std::atomic<std::uint64_t> dropped{0};
  LossFilter(std::uint64_t seed, double prob) : rng(seed), p(prob) {}
  bool operator()(const std::uint8_t*, std::size_t) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    if (u(rng) < p) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    return true;
  }
};

}  // namespace

TEST(realnet, "a 4 MB transfer over loopback UDP is byte-exact") {
  auto data = make_data(4 * 1024 * 1024, 1);
  UdpSocket rx;
  rx.bind_to(Address::loopback(0));
  const auto peer = rx.local_address();

  std::vector<std::uint8_t> got;
  RunResult rres, sres;
  std::thread rt([&] { rres = recv_stream_on(rx, &got, runner_config()); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  sres = send_stream(peer, data.data(), data.size(), runner_config());
  rt.join();

  CHECK_MSG(sres.ok, "send failed: " + sres.error);
  CHECK_MSG(rres.ok, "recv failed: " + rres.error);
  CHECK_EQ(got.size(), data.size());
  CHECK(got == data);
  CHECK_EQ(sres.sha256, rres.sha256);
  CHECK_EQ(rres.rcv.data_bytes_delivered, data.size());
  CHECK(sres.rtt.samples > 0);
}

TEST(realnet, "an empty transfer over a real socket completes") {
  std::vector<std::uint8_t> data;
  UdpSocket rx;
  rx.bind_to(Address::loopback(0));
  const auto peer = rx.local_address();
  std::vector<std::uint8_t> got;
  RunResult rres, sres;
  std::thread rt([&] { rres = recv_stream_on(rx, &got, runner_config(16)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  sres = send_stream(peer, nullptr, 0, runner_config(16));
  rt.join();
  CHECK_MSG(sres.ok, "send failed: " + sres.error);
  CHECK_MSG(rres.ok, "recv failed: " + rres.error);
  CHECK_EQ(got.size(), static_cast<std::size_t>(0));
  CHECK_EQ(sres.sha256, rres.sha256);
}

TEST(realnet, "10%% injected loss on both directions of a real socket recovers") {
  auto data = make_data(1024 * 1024, 2);
  UdpSocket rx;
  rx.bind_to(Address::loopback(0));
  const auto peer = rx.local_address();

  LossFilter rx_loss(11, 0.10);
  LossFilter tx_loss(22, 0.10);
  std::vector<std::uint8_t> got;
  RunResult rres, sres;
  std::thread rt([&] {
    rres = recv_stream_on(rx, &got, runner_config(64),
                          [&](const std::uint8_t* p, std::size_t n) { return rx_loss(p, n); });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  sres = send_stream(peer, data.data(), data.size(), runner_config(64),
                     [&](const std::uint8_t* p, std::size_t n) { return tx_loss(p, n); });
  rt.join();

  CHECK_MSG(sres.ok, "send failed: " + sres.error);
  CHECK_MSG(rres.ok, "recv failed: " + rres.error);
  CHECK(got == data);
  CHECK_EQ(sres.sha256, rres.sha256);
  CHECK(rx_loss.dropped.load() > 50);
  CHECK(tx_loss.dropped.load() > 5);
  CHECK(sres.snd.retransmits > 50);
}

TEST(realnet, "a receiver with no peer gives up instead of hanging") {
  RunnerConfig rc = runner_config(8);
  rc.idle_timeout_us = 300000;  // 0.3 s
  std::vector<std::uint8_t> got;
  const auto t0 = now_us();
  auto res = recv_stream(Address::loopback(0), &got, rc);
  const auto elapsed = now_us() - t0;
  CHECK(!res.ok);
  CHECK(res.error.find("idle timeout") != std::string::npos);
  CHECK(elapsed < 5000000);
}

TEST(realnet, "a sender to a dead port gives up instead of hanging") {
  RunnerConfig rc = runner_config(8);
  rc.idle_timeout_us = 500000;
  rc.proto.handshake_retries = 3;
  rc.proto.init_rto_us = 20000;
  rc.proto.normalize();
  // Bind and immediately release, so the port is very likely unused.
  std::uint16_t port = 0;
  {
    UdpSocket s;
    s.bind_to(Address::loopback(0));
    port = s.local_address().port();
  }
  std::vector<std::uint8_t> data(1024, 7);
  const auto t0 = now_us();
  auto res = send_stream(Address::loopback(port), data.data(), data.size(), rc);
  CHECK(!res.ok);
  CHECK(now_us() - t0 < 5000000);
}

TEST(realnet, "the server accepts a single session and hashes it correctly") {
  auto data = make_data(2 * 1024 * 1024, 3);
  ServerConfig sc;
  sc.proto = runner_config().proto;
  Server server(Address::loopback(0), sc);
  const auto addr = server.local_address();
  server.start();

  auto sres = send_stream(addr, data.data(), data.size(), runner_config());
  CHECK_MSG(sres.ok, "send failed: " + sres.error);

  std::vector<SessionResult> done;
  const auto deadline = now_us() + 10000000;
  while (done.empty() && now_us() < deadline) {
    for (auto& r : server.take_completed()) done.push_back(std::move(r));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  server.stop();
  CHECK_EQ(done.size(), static_cast<std::size_t>(1));
  CHECK(done[0].ok);
  CHECK_EQ(done[0].bytes, data.size());
  CHECK_EQ(done[0].sha256, sha256_hex(data.data(), data.size()));
}

TEST(realnet, "the server runs 8 concurrent sessions with isolated state") {
  const int kSessions = 8;
  const std::size_t kBytes = 768 * 1024;
  ServerConfig sc;
  sc.proto = runner_config(64).proto;
  Server server(Address::loopback(0), sc);
  const auto addr = server.local_address();
  server.start();

  std::vector<std::vector<std::uint8_t>> payloads;
  std::vector<std::string> hashes;
  for (int i = 0; i < kSessions; ++i) {
    payloads.push_back(make_data(kBytes, static_cast<std::uint64_t>(100 + i)));
    hashes.push_back(sha256_hex(payloads.back().data(), payloads.back().size()));
  }

  std::vector<std::thread> senders;
  std::vector<RunResult> results(kSessions);
  for (int i = 0; i < kSessions; ++i) {
    senders.emplace_back([&, i] {
      RunnerConfig rc = runner_config(64);
      // Distinct session ids so the server's demultiplexing is exercised even
      // though the peer addresses also differ.
      rc.proto.session_id = 0xA0000000u + static_cast<std::uint32_t>(i);
      rc.proto.isn = static_cast<seq_t>(1000 * (i + 1));
      results[static_cast<std::size_t>(i)] =
          send_stream(addr, payloads[static_cast<std::size_t>(i)].data(), kBytes, rc);
    });
  }
  for (auto& t : senders) t.join();

  std::vector<SessionResult> done;
  const auto deadline = now_us() + 20000000;
  while (done.size() < static_cast<std::size_t>(kSessions) && now_us() < deadline) {
    for (auto& r : server.take_completed()) done.push_back(std::move(r));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  server.stop();

  for (int i = 0; i < kSessions; ++i) {
    CHECK_MSG(results[static_cast<std::size_t>(i)].ok,
              "sender " + std::to_string(i) + " failed: " +
                  results[static_cast<std::size_t>(i)].error);
  }
  CHECK_EQ(done.size(), static_cast<std::size_t>(kSessions));
  // Every session must have produced exactly its own stream: no cross-talk.
  std::vector<bool> matched(kSessions, false);
  for (const auto& r : done) {
    CHECK(r.ok);
    CHECK_EQ(r.bytes, kBytes);
    bool found = false;
    for (int i = 0; i < kSessions; ++i) {
      if (!matched[static_cast<std::size_t>(i)] && r.sha256 == hashes[static_cast<std::size_t>(i)]) {
        matched[static_cast<std::size_t>(i)] = true;
        found = true;
        break;
      }
    }
    CHECK_MSG(found, "server session hash matched no client stream (state leaked between "
                     "sessions?)");
  }
}

TEST(realnet, "the server rejects an unknown session with RST") {
  ServerConfig sc;
  sc.proto = runner_config(8).proto;
  Server server(Address::loopback(0), sc);
  const auto addr = server.local_address();
  server.start();

  // A DATA packet with no preceding SYN.
  UdpSocket s;
  s.bind_to(Address::loopback(0));
  s.set_nonblocking(true);
  Header h;
  h.type = PacketType::Data;
  h.session_id = 0x99999999;
  h.seq = 5;
  std::vector<std::uint8_t> payload(16, 1);
  std::vector<std::uint8_t> buf(kMaxOverhead);
  const std::size_t hl =
      encode_packet(buf.data(), h, nullptr, 0, payload.data(), payload.size());
  CHECK(s.send_gather(buf.data(), hl, payload.data(), payload.size(), &addr));

  bool got_rst = false;
  const auto deadline = now_us() + 2000000;
  std::vector<std::uint8_t> in(2048);
  while (!got_rst && now_us() < deadline) {
    if (!s.wait_readable(50000)) continue;
    Address from;
    const auto n = s.recv_from(in.data(), in.size(), &from);
    if (n < static_cast<std::ptrdiff_t>(kHeaderSize)) continue;
    Header rh;
    SackBlock sk[kMaxSackBlocks];
    std::size_t ns = 0;
    const std::uint8_t* p = nullptr;
    std::size_t pl = 0;
    if (parse_packet(in.data(), static_cast<std::size_t>(n), rh, sk, ns, p, pl) ==
            ParseResult::Ok &&
        rh.type == PacketType::Rst) {
      got_rst = true;
    }
  }
  server.stop();
  CHECK(got_rst);
  CHECK(server.rx_unknown_session() >= 1);
}
