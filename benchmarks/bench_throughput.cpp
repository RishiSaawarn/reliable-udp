// Wall-clock throughput over real UDP sockets on the loopback interface.
//
// What this measures: how fast this implementation can push a file through a
// real kernel UDP path on this machine, including syscall cost, the ACK path,
// and whatever loss the socket receive buffer imposes when it overflows.
// It is NOT a measure of performance over a real WAN; see docs/CV_AUDIT.md.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "loopback.hpp"
#include "reliable_udp/sim/sim_harness.hpp"

using namespace rudp;

int main(int argc, char** argv) {
  std::uint32_t window = 256;
  std::uint16_t payload = 1200;
  std::string csv_path = "results/throughput.csv";
  int scale = 1;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--window") == 0) window = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    else if (std::strcmp(argv[i], "--payload") == 0) payload = static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
    else if (std::strcmp(argv[i], "--csv") == 0) csv_path = argv[i + 1];
    else if (std::strcmp(argv[i], "--scale") == 0) scale = std::atoi(argv[i + 1]);
  }

  const auto env = bench::capture_env();
  bench::print_env(env);

  struct Case { const char* name; std::size_t bytes; int reps; };
  const Case cases[] = {
      {"1 MB",   1ull * 1024 * 1024,   9},
      {"10 MB",  10ull * 1024 * 1024,  9},
      {"100 MB", 100ull * 1024 * 1024, 5},
  };

  rudp::net::RunnerConfig rc;
  rc.proto.max_payload = payload;
  rc.proto.send_window = window;
  rc.proto.recv_window = window * 2;
  rc.proto.min_rto_us = 2000;
  rc.proto.init_rto_us = 50000;
  rc.proto.linger_us = 50000;
  rc.proto.normalize();
  rc.rcvbuf_bytes = 32 << 20;
  rc.sndbuf_bytes = 32 << 20;
  rc.poll_slice_us = 2000;
  rc.idle_timeout_us = 60ull * 1000 * 1000;

  std::printf("transport: real UDP over loopback (127.0.0.1), sender and receiver threads\n");
  std::printf("packet=%u B payload, send_window=%u pkts, recv_window=%u pkts\n",
              rc.proto.max_payload, rc.proto.send_window, rc.proto.recv_window);
  std::printf("SO_RCVBUF requested=%d B, SO_SNDBUF requested=%d B\n", rc.rcvbuf_bytes,
              rc.sndbuf_bytes);
  std::printf("1 warm-up run per size, discarded; %d/%d/%d measured repetitions\n\n",
              cases[0].reps, cases[1].reps, cases[2].reps);

  bench::Csv csv(csv_path);
  csv.header(bench::env_csv_header() +
             ",size_name,payload_bytes,packet_bytes,send_window,recv_window,rep,ok,sha_ok,"
             "wall_us,goodput_mbps,data_pkts_sent,retransmits,retx_pct,fast_retx,timeout_retx,"
             "kernel_missing_datagrams,acks_received,mean_rtt_us,min_rtt_us,max_rtt_us,"
             "srtt_us,rttvar_us,final_rto_us,cpu_user_us,cpu_sys_us,cpu_per_MB_ms,"
             "socket_rcvbuf,rx_bad_checksum");

  std::printf("%-8s %8s %11s %11s %10s %8s %8s %10s %9s %9s\n", "size", "runs", "time(ms)",
              "goodput", "pkts", "retx", "retx%", "kern-drop", "RTT(us)", "cpu/MB");
  std::printf("%s\n", std::string(105, '-').c_str());

  int failures = 0;
  for (const Case& c : cases) {
    const std::size_t bytes = c.bytes / static_cast<std::size_t>(scale);
    std::vector<std::uint8_t> data(bytes);
    sim::fill_pattern(data, 0x1234);

    // Warm-up: pages faulted in, CPU frequency ramped, caches primed.
    (void)bench::run_loopback(data, rc, rc, false);

    std::vector<double> t_ms, gp, retx_pct, cpu_per_mb, rtt;
    std::vector<double> pkts, retx, drops;
    for (int rep = 0; rep < c.reps; ++rep) {
      auto r = bench::run_loopback(data, rc, rc, /*verify_bytes=*/rep == 0);
      if (!r.ok || !r.sha_ok) {
        ++failures;
        std::printf("  !! FAILURE %s rep %d: %s (sha_ok=%d)\n", c.name, rep, r.error.c_str(),
                    r.sha_ok ? 1 : 0);
      }
      const double ms = static_cast<double>(r.wall_us) / 1000.0;
      const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
      t_ms.push_back(ms);
      gp.push_back(bench::mbps(bytes, r.wall_us));
      retx_pct.push_back(r.sender.snd.data_packets_sent
                             ? 100.0 * static_cast<double>(r.sender.snd.retransmits) /
                                   static_cast<double>(r.sender.snd.data_packets_sent)
                             : 0.0);
      cpu_per_mb.push_back(static_cast<double>(r.cpu.total_us()) / 1000.0 / mb);
      rtt.push_back(r.sender.rtt.mean_us);
      pkts.push_back(static_cast<double>(r.sender.snd.data_packets_sent));
      retx.push_back(static_cast<double>(r.sender.snd.retransmits));
      drops.push_back(static_cast<double>(r.missing_datagrams));

      csv.row(bench::env_csv_row(env) +
              bench::fmt(",\"%s\",%zu,%u,%u,%u,%d,%d,%d,%llu,%.4f,%llu,%llu,%.4f,%llu,%llu,"
                         "%llu,%llu,%.2f,%llu,%llu,%.2f,%.2f,%llu,%llu,%llu,%.4f,%llu,%llu",
                         c.name, bytes, rc.proto.max_payload, rc.proto.send_window,
                         rc.proto.recv_window, rep, r.ok ? 1 : 0, r.sha_ok ? 1 : 0,
                         static_cast<unsigned long long>(r.wall_us),
                         bench::mbps(bytes, r.wall_us),
                         static_cast<unsigned long long>(r.sender.snd.data_packets_sent),
                         static_cast<unsigned long long>(r.sender.snd.retransmits),
                         retx_pct.back(),
                         static_cast<unsigned long long>(r.sender.snd.fast_retransmits),
                         static_cast<unsigned long long>(r.sender.snd.timeout_retransmits),
                         static_cast<unsigned long long>(r.missing_datagrams),
                         static_cast<unsigned long long>(r.sender.snd.acks_received),
                         r.sender.rtt.mean_us,
                         static_cast<unsigned long long>(r.sender.rtt.min_us),
                         static_cast<unsigned long long>(r.sender.rtt.max_us),
                         r.sender.rtt.srtt_us, r.sender.rtt.rttvar_us,
                         static_cast<unsigned long long>(r.sender.rtt.final_rto_us),
                         static_cast<unsigned long long>(r.cpu.user_us),
                         static_cast<unsigned long long>(r.cpu.sys_us), cpu_per_mb.back(),
                         static_cast<unsigned long long>(r.sender.socket_rcvbuf),
                         static_cast<unsigned long long>(r.receiver.rcv.rx_bad_checksum)));
    }

    std::printf("%-8s %8d %11.1f %8.1f Mb/s %10.0f %8.0f %8.3f %10.0f %9.0f %8.2f ms\n", c.name,
                c.reps, bench::summarise(t_ms).median, bench::summarise(gp).median,
                bench::summarise(pkts).median, bench::summarise(retx).median,
                bench::summarise(retx_pct).median, bench::summarise(drops).median,
                bench::summarise(rtt).median, bench::summarise(cpu_per_mb).median);
    std::printf("           goodput  %s\n", bench::summarise(gp).str().c_str());
    std::printf("           time(ms) %s\n", bench::summarise(t_ms).str().c_str());
    std::printf("           retx%%    %s\n", bench::summarise(retx_pct).str().c_str());
  }

  std::printf("\nper-repetition data: %s\n", csv.path().c_str());
  std::printf("%d failing repetition(s).\n", failures);
  return failures == 0 ? 0 : 1;
}
