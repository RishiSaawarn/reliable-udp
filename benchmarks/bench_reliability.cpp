// Reliability matrix: does the transfer complete, and byte-exactly, across a
// grid of loss rates and link conditions? Run in the deterministic simulator,
// so every cell is reproducible from its seed.
//
// IMPORTANT about the goodput column: the simulator advances a *virtual* clock,
// so "goodput" here is the protocol's efficiency against the simulated link
// (delay, jitter, rate limit), not a wall-clock measurement of this machine.
// Wall-clock throughput is measured separately in bench_throughput.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "reliable_udp/sim/sim_harness.hpp"

using namespace rudp;
using namespace rudp::sim;

namespace {

struct Cell {
  std::string name;
  double loss;
  double ack_loss;
  double dup;
  double reorder;
  std::uint64_t delay_us;
  std::uint64_t jitter_us;
};

struct RepResult {
  bool completed;
  bool sha_ok;
  double virtual_ms;
  double goodput_mbps;
  double retx_pct;
  std::uint64_t retransmits;
  std::uint64_t fast_retx;
  std::uint64_t timeout_retx;
  std::uint64_t sim_dropped_fwd;
  std::uint64_t sim_dropped_rev;
  std::uint64_t sim_dup;
  std::uint64_t sim_reordered;
  std::uint64_t data_pkts;
  double mean_rtt_us;
  std::uint64_t final_rto_us;
  std::uint64_t rcv_dup;
  std::uint64_t rcv_ooo;
  double sim_cpu_ms;
};

}  // namespace

int main(int argc, char** argv) {
  std::size_t payload_bytes = 4 * 1024 * 1024;
  int reps = 9;
  std::uint32_t window = 64;
  std::string csv_path = "results/reliability.csv";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--bytes") == 0) payload_bytes = static_cast<std::size_t>(std::atoll(argv[i + 1]));
    else if (std::strcmp(argv[i], "--reps") == 0) reps = std::atoi(argv[i + 1]);
    else if (std::strcmp(argv[i], "--window") == 0) window = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    else if (std::strcmp(argv[i], "--csv") == 0) csv_path = argv[i + 1];
  }

  const auto env = bench::capture_env();
  bench::print_env(env);

  Config proto;
  proto.max_payload = 1200;
  proto.send_window = window;
  proto.recv_window = window * 2;
  proto.min_rto_us = 5000;
  proto.init_rto_us = 200000;
  proto.linger_us = 100000;
  proto.normalize();

  std::vector<std::uint8_t> data(payload_bytes);
  fill_pattern(data, 0xABCDEF);

  // 25 ms one-way / 50 ms RTT with 5 ms jitter is the "realistic delay"
  // configuration; the 0 ms rows isolate the effect of loss alone.
  const std::vector<Cell> cells = {
      {"loss 0%  / no delay",        0.00, 0.00, 0.00, 0.00, 0,     0},
      {"loss 1%  / no delay",        0.01, 0.00, 0.00, 0.00, 0,     0},
      {"loss 5%  / no delay",        0.05, 0.00, 0.00, 0.00, 0,     0},
      {"loss 10% / no delay",        0.10, 0.00, 0.00, 0.00, 0,     0},
      {"loss 20% / no delay",        0.20, 0.00, 0.00, 0.00, 0,     0},
      {"loss 0%  / 50ms RTT",        0.00, 0.00, 0.00, 0.00, 25000, 5000},
      {"loss 1%  / 50ms RTT",        0.01, 0.005,0.00, 0.00, 25000, 5000},
      {"loss 5%  / 50ms RTT",        0.05, 0.025,0.00, 0.00, 25000, 5000},
      {"loss 10% / 50ms RTT",        0.10, 0.05, 0.00, 0.00, 25000, 5000},
      {"loss 20% / 50ms RTT",        0.20, 0.10, 0.00, 0.00, 25000, 5000},
      {"loss 5%  + 10% dup",         0.05, 0.025,0.10, 0.00, 25000, 5000},
      {"loss 5%  + 30% reorder",     0.05, 0.025,0.00, 0.30, 25000, 5000},
      {"loss 5%  + dup + reorder",   0.05, 0.025,0.10, 0.30, 25000, 5000},
      {"loss 10% + dup + reorder",   0.10, 0.05, 0.10, 0.30, 25000, 5000},
      {"loss 20% + dup + reorder",   0.20, 0.10, 0.10, 0.30, 25000, 5000},
  };

  bench::Csv csv(csv_path);
  csv.header(bench::env_csv_header() +
             ",scenario,payload_bytes,packet_bytes,window,recv_window,loss,ack_loss,dup,"
             "reorder,delay_us,jitter_us,seed,rep,completed,sha_ok,virtual_ms,goodput_mbps,"
             "data_pkts,retransmits,retx_pct,fast_retx,timeout_retx,sim_dropped_fwd,"
             "sim_dropped_rev,sim_dup,sim_reordered,rcv_dup,rcv_out_of_order,mean_rtt_us,"
             "final_rto_us,sim_cpu_ms");

  std::printf("payload=%zu B, packet=%u B, window=%u pkts, recv_window=%u pkts, reps=%d\n",
              payload_bytes, proto.max_payload, proto.send_window, proto.recv_window, reps);
  std::printf("seeds: 1000+rep (deterministic)\n\n");
  std::printf("%-28s %6s %10s %10s %9s %9s %9s %10s %9s\n", "scenario", "ok",
              "time(ms)", "gp(Mb/s)", "pkts", "retx", "retx%", "meanRTT", "RTO(ms)");
  std::printf("%s\n", std::string(118, '-').c_str());

  int failures = 0;
  for (const Cell& c : cells) {
    std::vector<RepResult> results;
    for (int rep = 0; rep < reps; ++rep) {
      SimParams p;
      p.cfg = proto;
      p.seed = 1000 + static_cast<std::uint64_t>(rep);
      p.fwd.loss_prob = c.loss;
      p.fwd.dup_prob = c.dup;
      p.fwd.reorder_prob = c.reorder;
      p.fwd.delay_us = c.delay_us;
      p.fwd.jitter_us = c.jitter_us;
      p.fwd.reorder_extra_us = c.delay_us + 10000;
      p.rev.loss_prob = c.ack_loss;
      p.rev.delay_us = c.delay_us;
      p.rev.jitter_us = c.jitter_us;
      p.max_virtual_us = 3600ull * 1000 * 1000;

      std::vector<std::uint8_t> out;
      const auto cpu0 = bench::cpu_time();
      const auto rep_report = run_sim_transfer(data, p, &out);
      const auto cpu1 = bench::cpu_time();

      RepResult r{};
      r.completed = rep_report.completed;
      r.sha_ok = rep_report.sha_sent == rep_report.sha_received && out.size() == data.size();
      r.virtual_ms = static_cast<double>(rep_report.virtual_us) / 1000.0;
      r.goodput_mbps = rep_report.goodput_mbps();
      r.retx_pct = rep_report.retransmit_pct();
      r.retransmits = rep_report.snd.retransmits;
      r.fast_retx = rep_report.snd.fast_retransmits;
      r.timeout_retx = rep_report.snd.timeout_retransmits;
      r.sim_dropped_fwd = rep_report.fwd.dropped_loss;
      r.sim_dropped_rev = rep_report.rev.dropped_loss;
      r.sim_dup = rep_report.fwd.duplicated;
      r.sim_reordered = rep_report.fwd.reordered;
      r.data_pkts = rep_report.snd.data_packets_sent;
      r.mean_rtt_us = rep_report.mean_rtt_us;
      r.final_rto_us = rep_report.final_rto_us;
      r.rcv_dup = rep_report.rcv.dup_data;
      r.rcv_ooo = rep_report.rcv.out_of_order;
      r.sim_cpu_ms = static_cast<double>(bench::cpu_delta(cpu0, cpu1).total_us()) / 1000.0;
      results.push_back(r);

      if (!r.completed || !r.sha_ok) {
        ++failures;
        std::printf("  !! FAILURE  scenario=%s seed=%llu : %s\n", c.name.c_str(),
                    static_cast<unsigned long long>(p.seed),
                    rep_report.completed ? "sha mismatch" : rep_report.failure.c_str());
      }

      csv.row(bench::env_csv_row(env) +
              bench::fmt(",\"%s\",%zu,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,%llu,%llu,%llu,%d,%d,%d,"
                         "%.3f,%.4f,%llu,%llu,%.4f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                         "%.1f,%llu,%.3f",
                         c.name.c_str(), payload_bytes, proto.max_payload, proto.send_window,
                         proto.recv_window, c.loss, c.ack_loss, c.dup, c.reorder,
                         static_cast<unsigned long long>(c.delay_us),
                         static_cast<unsigned long long>(c.jitter_us),
                         static_cast<unsigned long long>(p.seed), rep, r.completed ? 1 : 0,
                         r.sha_ok ? 1 : 0, r.virtual_ms, r.goodput_mbps,
                         static_cast<unsigned long long>(r.data_pkts),
                         static_cast<unsigned long long>(r.retransmits), r.retx_pct,
                         static_cast<unsigned long long>(r.fast_retx),
                         static_cast<unsigned long long>(r.timeout_retx),
                         static_cast<unsigned long long>(r.sim_dropped_fwd),
                         static_cast<unsigned long long>(r.sim_dropped_rev),
                         static_cast<unsigned long long>(r.sim_dup),
                         static_cast<unsigned long long>(r.sim_reordered),
                         static_cast<unsigned long long>(r.rcv_dup),
                         static_cast<unsigned long long>(r.rcv_ooo), r.mean_rtt_us,
                         static_cast<unsigned long long>(r.final_rto_us), r.sim_cpu_ms));
    }

    auto col = [&](double RepResult::*m) {
      std::vector<double> v;
      for (const auto& r : results) v.push_back(r.*m);
      return bench::summarise(v);
    };
    auto colu = [&](std::uint64_t RepResult::*m) {
      std::vector<double> v;
      for (const auto& r : results) v.push_back(static_cast<double>(r.*m));
      return bench::summarise(v);
    };
    int ok = 0;
    for (const auto& r : results) ok += (r.completed && r.sha_ok) ? 1 : 0;

    std::printf("%-28s %3d/%-2d %10.1f %10.2f %9.0f %9.0f %9.3f %10.0f %9.1f\n", c.name.c_str(),
                ok, reps, col(&RepResult::virtual_ms).median,
                col(&RepResult::goodput_mbps).median, colu(&RepResult::data_pkts).median,
                colu(&RepResult::retransmits).median, col(&RepResult::retx_pct).median,
                col(&RepResult::mean_rtt_us).median,
                colu(&RepResult::final_rto_us).median / 1000.0);
  }

  std::printf("\n--- distributions (median [min | p25 | p75 | max]) ---------------------------\n");
  std::printf("(full per-repetition data in %s)\n", csv.path().c_str());
  std::printf("\n%d failing repetition(s) out of %zu.\n", failures, cells.size() * static_cast<std::size_t>(reps));
  return failures == 0 ? 0 : 1;
}
