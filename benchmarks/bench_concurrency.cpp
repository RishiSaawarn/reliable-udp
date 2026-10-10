// Concurrent independent sessions against the threaded multi-session server.
//
// State isolation: each session owns its own SrReceiver, its own reassembly
// ring, its own UdpSink and its own SHA-256 context. Nothing protocol-related
// is shared between sessions -- only the socket (kernel-atomic per datagram),
// the session table (a shared_mutex, read-locked per datagram) and the
// completion queue. The benchmark verifies isolation by requiring every
// session's SHA-256 to match exactly one distinct client stream.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "reliable_udp/net/server.hpp"
#include "reliable_udp/net/session_runner.hpp"
#include "reliable_udp/sha256.hpp"
#include "reliable_udp/sim/sim_harness.hpp"

using namespace rudp;
using namespace rudp::net;

namespace {

struct RunOutcome {
  bool ok = false;
  std::uint64_t wall_us = 0;          // wall time for the whole batch
  double aggregate_mbps = 0;
  double per_session_median_mbps = 0;
  std::uint64_t retransmits = 0;
  std::uint64_t data_pkts = 0;
  double mean_rtt_us = 0;
  bench::CpuTime cpu;
  int sessions_verified = 0;
};

RunOutcome run_batch(int nsessions, std::size_t bytes_each, const RunnerConfig& rc,
                     const ServerConfig& sc) {
  RunOutcome o;
  Server server(Address::loopback(0), sc);
  const Address addr = server.local_address();
  server.start();

  std::vector<std::vector<std::uint8_t>> payloads(static_cast<std::size_t>(nsessions));
  std::vector<std::string> hashes(static_cast<std::size_t>(nsessions));
  for (int i = 0; i < nsessions; ++i) {
    payloads[static_cast<std::size_t>(i)].resize(bytes_each);
    sim::fill_pattern(payloads[static_cast<std::size_t>(i)], static_cast<std::uint64_t>(9000 + i));
    hashes[static_cast<std::size_t>(i)] =
        sha256_hex(payloads[static_cast<std::size_t>(i)].data(), bytes_each);
  }

  std::vector<RunResult> results(static_cast<std::size_t>(nsessions));
  std::vector<std::thread> threads;
  const bench::CpuTime c0 = bench::cpu_time();
  const std::uint64_t t0 = bench::wall_us();
  for (int i = 0; i < nsessions; ++i) {
    threads.emplace_back([&, i] {
      RunnerConfig my = rc;
      my.proto.session_id = 0xB0000000u + static_cast<std::uint32_t>(i);
      my.proto.isn = static_cast<seq_t>(1 + 7919u * static_cast<std::uint32_t>(i));
      my.proto.normalize();
      results[static_cast<std::size_t>(i)] =
          send_stream(addr, payloads[static_cast<std::size_t>(i)].data(), bytes_each, my);
    });
  }
  for (auto& t : threads) t.join();
  const std::uint64_t t1 = bench::wall_us();
  const bench::CpuTime c1 = bench::cpu_time();

  std::vector<SessionResult> done;
  const std::uint64_t deadline = now_us() + 30000000;
  while (done.size() < static_cast<std::size_t>(nsessions) && now_us() < deadline) {
    for (auto& r : server.take_completed()) done.push_back(std::move(r));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  server.stop();

  o.wall_us = t1 - t0;
  o.cpu = bench::cpu_delta(c0, c1);
  o.ok = done.size() == static_cast<std::size_t>(nsessions);
  std::vector<double> per;
  double rtt_sum = 0;
  for (const auto& r : results) {
    if (!r.ok) o.ok = false;
    o.retransmits += r.snd.retransmits;
    o.data_pkts += r.snd.data_packets_sent;
    rtt_sum += r.rtt.mean_us;
    per.push_back(bench::mbps(bytes_each, r.wall_us));
  }
  o.mean_rtt_us = rtt_sum / static_cast<double>(nsessions);
  o.per_session_median_mbps = bench::summarise(per).median;
  o.aggregate_mbps = bench::mbps(bytes_each * static_cast<std::size_t>(nsessions), o.wall_us);

  std::vector<bool> matched(static_cast<std::size_t>(nsessions), false);
  for (const auto& r : done) {
    if (!r.ok || r.bytes != bytes_each) { o.ok = false; continue; }
    for (int i = 0; i < nsessions; ++i) {
      if (!matched[static_cast<std::size_t>(i)] && r.sha256 == hashes[static_cast<std::size_t>(i)]) {
        matched[static_cast<std::size_t>(i)] = true;
        ++o.sessions_verified;
        break;
      }
    }
  }
  if (o.sessions_verified != nsessions) o.ok = false;
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  int reps = 5;
  std::size_t bytes_each = 8 * 1024 * 1024;
  std::uint32_t window = 256;
  std::string csv_path = "results/concurrency.csv";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--reps") == 0) reps = std::atoi(argv[i + 1]);
    else if (std::strcmp(argv[i], "--bytes") == 0) bytes_each = static_cast<std::size_t>(std::atoll(argv[i + 1]));
    else if (std::strcmp(argv[i], "--window") == 0) window = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    else if (std::strcmp(argv[i], "--csv") == 0) csv_path = argv[i + 1];
  }

  const auto env = bench::capture_env();
  bench::print_env(env);

  RunnerConfig rc;
  rc.proto.max_payload = 1200;
  rc.proto.send_window = window;
  rc.proto.recv_window = window * 2;
  rc.proto.min_rto_us = 2000;
  rc.proto.init_rto_us = 50000;
  rc.proto.linger_us = 50000;
  rc.proto.normalize();
  rc.rcvbuf_bytes = 16 << 20;
  rc.sndbuf_bytes = 16 << 20;
  rc.idle_timeout_us = 60ull * 1000 * 1000;

  ServerConfig sc;
  sc.proto = rc.proto;
  sc.rcvbuf_bytes = 32 << 20;
  sc.sndbuf_bytes = 32 << 20;
  sc.tick_interval_us = 200;

  std::printf("server: 1 UDP socket, 1 rx thread, 1 timer thread, %zu MiB per session\n",
              bytes_each / (1024 * 1024));
  std::printf("clients: one thread and one socket each; %d measured repetitions + 1 warm-up\n\n",
              reps);

  bench::Csv csv(csv_path);
  csv.header(bench::env_csv_header() +
             ",sessions,bytes_each,window,rep,ok,sessions_verified,wall_us,aggregate_mbps,"
             "per_session_median_mbps,data_pkts,retransmits,retx_pct,mean_rtt_us,cpu_user_us,"
             "cpu_sys_us,cpu_per_MB_ms");

  std::printf("%-10s %7s %11s %14s %16s %9s %10s %10s\n", "sessions", "ok", "wall(ms)",
              "aggregate", "per-session", "retx%", "meanRTT", "cpu/MB");
  std::printf("%s\n", std::string(96, '-').c_str());

  int failures = 0;
  for (int n : {1, 2, 4, 8}) {
    (void)run_batch(n, bytes_each / 4, rc, sc);  // warm-up
    std::vector<double> agg, per, wall, rtxp, rtt, cpumb;
    int ok_runs = 0;
    for (int rep = 0; rep < reps; ++rep) {
      auto o = run_batch(n, bytes_each, rc, sc);
      if (!o.ok) {
        ++failures;
        std::printf("  !! FAILURE sessions=%d rep=%d (verified %d/%d)\n", n, rep,
                    o.sessions_verified, n);
      } else {
        ++ok_runs;
      }
      const double total_mb =
          static_cast<double>(bytes_each) * static_cast<double>(n) / (1024.0 * 1024.0);
      const double rp = o.data_pkts ? 100.0 * static_cast<double>(o.retransmits) /
                                          static_cast<double>(o.data_pkts)
                                    : 0.0;
      agg.push_back(o.aggregate_mbps);
      per.push_back(o.per_session_median_mbps);
      wall.push_back(static_cast<double>(o.wall_us) / 1000.0);
      rtxp.push_back(rp);
      rtt.push_back(o.mean_rtt_us);
      cpumb.push_back(static_cast<double>(o.cpu.total_us()) / 1000.0 / total_mb);
      csv.row(bench::env_csv_row(env) +
              bench::fmt(",%d,%zu,%u,%d,%d,%d,%llu,%.4f,%.4f,%llu,%llu,%.4f,%.1f,%llu,%llu,%.4f",
                         n, bytes_each, rc.proto.send_window, rep, o.ok ? 1 : 0,
                         o.sessions_verified, static_cast<unsigned long long>(o.wall_us),
                         o.aggregate_mbps, o.per_session_median_mbps,
                         static_cast<unsigned long long>(o.data_pkts),
                         static_cast<unsigned long long>(o.retransmits), rp, o.mean_rtt_us,
                         static_cast<unsigned long long>(o.cpu.user_us),
                         static_cast<unsigned long long>(o.cpu.sys_us), cpumb.back()));
    }
    std::printf("%-10d %4d/%-2d %11.1f %10.1f Mb/s %11.1f Mb/s %9.3f %10.0f %8.2f ms\n", n,
                ok_runs, reps, bench::summarise(wall).median, bench::summarise(agg).median,
                bench::summarise(per).median, bench::summarise(rtxp).median,
                bench::summarise(rtt).median, bench::summarise(cpumb).median);
    std::printf("           aggregate %s\n", bench::summarise(agg).str().c_str());
  }

  std::printf("\nper-repetition data: %s\n", csv.path().c_str());
  std::printf("%d failing repetition(s).\n", failures);
  return failures == 0 ? 0 : 1;
}
