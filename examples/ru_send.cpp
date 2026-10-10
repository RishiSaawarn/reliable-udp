// Send a file over reliable-UDP.
//   ru_send <host> <port> <file> [--window N] [--payload N]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "reliable_udp/net/session_runner.hpp"

static bool read_file(const char* path, std::vector<std::uint8_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n < 0) { std::fclose(f); return false; }
  out.resize(static_cast<std::size_t>(n));
  const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <host> <port> <file> [--window N] [--payload N]\n", argv[0]);
    return 2;
  }
  rudp::net::RunnerConfig rc;
  for (int i = 4; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--window") == 0) {
      rc.proto.send_window = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    } else if (std::strcmp(argv[i], "--payload") == 0) {
      rc.proto.max_payload = static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
    }
  }
  rc.proto.normalize();

  std::vector<std::uint8_t> data;
  if (!read_file(argv[3], data)) {
    std::fprintf(stderr, "cannot read %s\n", argv[3]);
    return 1;
  }
  const auto peer = rudp::net::Address::parse(argv[1], static_cast<std::uint16_t>(std::atoi(argv[2])));
  const auto res = rudp::net::send_stream(peer, data.data(), data.size(), rc);
  if (!res.ok) {
    std::fprintf(stderr, "send failed: %s\n", res.error.c_str());
    return 1;
  }
  const double secs = static_cast<double>(res.wall_us) / 1e6;
  std::printf("sent %zu bytes in %.3f s = %.2f Mbit/s goodput  sha256=%s\n", res.bytes, secs,
              static_cast<double>(res.bytes) * 8.0 / static_cast<double>(res.wall_us),
              res.sha256.c_str());
  std::printf("  data pkts=%llu retx=%llu (%.3f%%) fast=%llu timeout=%llu\n",
              static_cast<unsigned long long>(res.snd.data_packets_sent),
              static_cast<unsigned long long>(res.snd.retransmits),
              res.snd.data_packets_sent
                  ? 100.0 * static_cast<double>(res.snd.retransmits) /
                        static_cast<double>(res.snd.data_packets_sent)
                  : 0.0,
              static_cast<unsigned long long>(res.snd.fast_retransmits),
              static_cast<unsigned long long>(res.snd.timeout_retransmits));
  std::printf("  rtt: samples=%llu srtt=%.1fus rttvar=%.1fus min=%lluus max=%lluus rto=%lluus\n",
              static_cast<unsigned long long>(res.rtt.samples), res.rtt.srtt_us,
              res.rtt.rttvar_us, static_cast<unsigned long long>(res.rtt.min_us),
              static_cast<unsigned long long>(res.rtt.max_us),
              static_cast<unsigned long long>(res.rtt.final_rto_us));
  return 0;
}
