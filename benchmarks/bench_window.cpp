// Does a larger sliding window actually buy throughput?
//
// Two experiments over identical workloads:
//   (A) simulated 50 ms RTT, 100 Mbit/s rate-limited link, 1% loss. Here the
//       bandwidth-delay product is the binding constraint, so the window should
//       matter until it exceeds the BDP and then stop mattering.
//   (B) real loopback UDP, where the RTT is tens of microseconds. Here the BDP
//       is tiny and the window should stop mattering almost immediately; what
//       is being measured is syscall and ACK-processing cost.
// Reporting both is the point: the same knob has completely different value
// depending on the path, and only measurement tells you which regime you are in.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "loopback.hpp"
#include "reliable_udp/sim/sim_harness.hpp"

using namespace rudp;
using namespace rudp::sim;

int main(int argc, char** argv) {
  int reps = 9;
  std::size_t sim_bytes = 8 * 1024 * 1024;
  std::size_t real_bytes = 20 * 1024 * 1024;
  std::string csv_path = "results/window.csv";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--reps") == 0) reps = std::atoi(argv[i + 1]);
    else if (std::strcmp(argv[i], "--csv") == 0) csv_path = argv[i + 1];
    else if (std::strcmp(argv[i], "--sim-bytes") == 0) sim_bytes = static_cast<std::size_t>(std::atoll(argv[i + 1]));
    else if (std::strcmp(argv[i], "--real-bytes") == 0) real_bytes = static_cast<std::size_t>(std::atoll(argv[i + 1]));
  }

  const auto env = bench::capture_env();
  bench::print_env(env);

  const std::uint32_t windows[] = {8, 16, 32, 64, 128};
  bench::Csv csv(csv_path);
  csv.header(bench::env_csv_header() +
             ",experiment,window,recv_window,payload_bytes,packet_bytes,rep,seed,ok,sha_ok,"
             "time_ms,goodput_mbps,data_pkts,retransmits,retx_pct,mean_rtt_us,final_rto_us,"
             "window_stalls,cpu_user_us,cpu_sys_us");

  // ------------------------------------------------------------- experiment A
  const std::uint64_t link_rate_bps = 100ull * 1000 * 1000;
  const std::uint64_t rtt_us = 50000;
  std::printf("=== (A) simulated link: 25 ms one-way delay (50 ms RTT), 100 Mbit/s, 1%% loss ===\n");
  std::printf("payload=%zu B, packet=1200 B, reps=%d, seeds 500+rep\n", sim_bytes, reps);
  {
    const double bdp_pkts =
        static_cast<double>(link_rate_bps) * (static_cast<double>(rtt_us) / 1e6) / 8.0 / 1200.0;
    std::printf("bandwidth-delay product = %.1f packets, so windows above ~%.0f should not help\n\n",
                bdp_pkts, bdp_pkts);
  }
  std::printf("%-8s %11s %12s %10s %10s %9s %10s %11s\n", "window", "time(ms)", "goodput",
              "pkts", "retx", "retx%", "meanRTT", "stalls");
  std::printf("%s\n", std::string(88, '-').c_str());

  std::vector<std::uint8_t> sdata(sim_bytes);
  fill_pattern(sdata, 0x5150);
  for (std::uint32_t w : windows) {
    std::vector<double> t_ms, gp, rtx_pct, rtt, pk, rx, stalls;
    for (int rep = 0; rep < reps; ++rep) {
      SimParams p;
      p.cfg.max_payload = 1200;
      p.cfg.send_window = w;
      p.cfg.recv_window = w * 2;
      p.cfg.min_rto_us = 5000;
      p.cfg.init_rto_us = 200000;
      p.cfg.linger_us = 100000;
      p.cfg.normalize();
      p.seed = 500 + static_cast<std::uint64_t>(rep);
      p.fwd.delay_us = rtt_us / 2;
      p.fwd.jitter_us = 2000;
      p.fwd.loss_prob = 0.01;
      p.fwd.rate_bps = link_rate_bps;
      p.rev.delay_us = rtt_us / 2;
      p.rev.jitter_us = 2000;
      p.max_virtual_us = 3600ull * 1000 * 1000;
      std::vector<std::uint8_t> out;
      const auto c0 = bench::cpu_time();
      auto rep_r = run_sim_transfer(sdata, p, &out);
      const auto c1 = bench::cpu_time();
      const bool sha_ok = rep_r.sha_sent == rep_r.sha_received;
      if (!rep_r.completed || !sha_ok) {
        std::printf("  !! FAILURE window=%u rep=%d: %s\n", w, rep, rep_r.failure.c_str());
      }
      t_ms.push_back(static_cast<double>(rep_r.virtual_us) / 1000.0);
      gp.push_back(rep_r.goodput_mbps());
      rtx_pct.push_back(rep_r.retransmit_pct());
      rtt.push_back(rep_r.mean_rtt_us);
      pk.push_back(static_cast<double>(rep_r.snd.data_packets_sent));
      rx.push_back(static_cast<double>(rep_r.snd.retransmits));
      stalls.push_back(static_cast<double>(rep_r.snd.window_stalls));
      const auto cpu = bench::cpu_delta(c0, c1);
      csv.row(bench::env_csv_row(env) +
              bench::fmt(",\"sim-50ms-100Mbit-1pct\",%u,%u,%zu,1200,%d,%llu,%d,%d,%.3f,%.4f,"
                         "%llu,%llu,%.4f,%.1f,%llu,%llu,%llu,%llu",
                         p.cfg.send_window, p.cfg.recv_window, sim_bytes, rep,
                         static_cast<unsigned long long>(p.seed), rep_r.completed ? 1 : 0,
                         sha_ok ? 1 : 0, t_ms.back(), gp.back(),
                         static_cast<unsigned long long>(rep_r.snd.data_packets_sent),
                         static_cast<unsigned long long>(rep_r.snd.retransmits),
                         rtx_pct.back(), rtt.back(),
                         static_cast<unsigned long long>(rep_r.final_rto_us),
                         static_cast<unsigned long long>(rep_r.snd.window_stalls),
                         static_cast<unsigned long long>(cpu.user_us),
                         static_cast<unsigned long long>(cpu.sys_us)));
    }
    std::printf("%-8u %11.1f %9.2f Mb/s %10.0f %10.0f %9.3f %10.0f %11.0f\n", w,
                bench::summarise(t_ms).median, bench::summarise(gp).median,
                bench::summarise(pk).median, bench::summarise(rx).median,
                bench::summarise(rtx_pct).median, bench::summarise(rtt).median,
                bench::summarise(stalls).median);
  }

  // ------------------------------------------------------------- experiment B
  std::printf("\n=== (B) real UDP over loopback (no artificial delay or loss) =================\n");
  std::printf("payload=%zu B, packet=1200 B, reps=%d, 1 warm-up discarded per window\n\n",
              real_bytes, reps);
  std::printf("%-8s %11s %12s %10s %10s %9s %10s %11s\n", "window", "time(ms)", "goodput",
              "pkts", "retx", "retx%", "meanRTT", "kern-drop");
  std::printf("%s\n", std::string(88, '-').c_str());

  std::vector<std::uint8_t> rdata(real_bytes);
  fill_pattern(rdata, 0x5150);
  for (std::uint32_t w : windows) {
    rudp::net::RunnerConfig rc;
    rc.proto.max_payload = 1200;
    rc.proto.send_window = w;
    rc.proto.recv_window = w * 2;
    rc.proto.min_rto_us = 2000;
    rc.proto.init_rto_us = 50000;
    rc.proto.linger_us = 50000;
    rc.proto.normalize();
    rc.rcvbuf_bytes = 32 << 20;
    rc.sndbuf_bytes = 32 << 20;
    rc.idle_timeout_us = 60ull * 1000 * 1000;

    (void)bench::run_loopback(rdata, rc, rc, false);  // warm-up

    std::vector<double> t_ms, gp, rtx_pct, rtt, pk, rx, drops;
    for (int rep = 0; rep < reps; ++rep) {
      auto r = bench::run_loopback(rdata, rc, rc, rep == 0);
      if (!r.ok || !r.sha_ok) std::printf("  !! FAILURE window=%u rep=%d: %s\n", w, rep, r.error.c_str());
      t_ms.push_back(static_cast<double>(r.wall_us) / 1000.0);
      gp.push_back(bench::mbps(real_bytes, r.wall_us));
      const double rp = r.sender.snd.data_packets_sent
                            ? 100.0 * static_cast<double>(r.sender.snd.retransmits) /
                                  static_cast<double>(r.sender.snd.data_packets_sent)
                            : 0.0;
      rtx_pct.push_back(rp);
      rtt.push_back(r.sender.rtt.mean_us);
      pk.push_back(static_cast<double>(r.sender.snd.data_packets_sent));
      rx.push_back(static_cast<double>(r.sender.snd.retransmits));
      drops.push_back(static_cast<double>(r.missing_datagrams));
      csv.row(bench::env_csv_row(env) +
              bench::fmt(",\"real-loopback\",%u,%u,%zu,1200,%d,0,%d,%d,%.3f,%.4f,%llu,%llu,"
                         "%.4f,%.1f,%llu,%llu,%llu,%llu",
                         rc.proto.send_window, rc.proto.recv_window, real_bytes, rep,
                         r.ok ? 1 : 0, r.sha_ok ? 1 : 0, t_ms.back(), gp.back(),
                         static_cast<unsigned long long>(r.sender.snd.data_packets_sent),
                         static_cast<unsigned long long>(r.sender.snd.retransmits), rp,
                         r.sender.rtt.mean_us,
                         static_cast<unsigned long long>(r.sender.rtt.final_rto_us),
                         static_cast<unsigned long long>(r.sender.snd.window_stalls),
                         static_cast<unsigned long long>(r.cpu.user_us),
                         static_cast<unsigned long long>(r.cpu.sys_us)));
    }
    std::printf("%-8u %11.1f %9.2f Mb/s %10.0f %10.0f %9.3f %10.0f %11.0f\n", w,
                bench::summarise(t_ms).median, bench::summarise(gp).median,
                bench::summarise(pk).median, bench::summarise(rx).median,
                bench::summarise(rtx_pct).median, bench::summarise(rtt).median,
                bench::summarise(drops).median);
    std::printf("         goodput %s\n", bench::summarise(gp).str().c_str());
  }

  std::printf("\nper-repetition data: %s\n", csv.path().c_str());
  return 0;
}
