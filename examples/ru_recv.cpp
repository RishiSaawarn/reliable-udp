// Receive a single stream over reliable-UDP and write it to a file.
//   ru_recv <port> <output-file> [--window N] [--payload N]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "reliable_udp/net/session_runner.hpp"
#include "reliable_udp/sha256.hpp"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <port> <output-file> [--window N] [--payload N]\n", argv[0]);
    return 2;
  }
  rudp::net::RunnerConfig rc;
  const auto port = static_cast<std::uint16_t>(std::atoi(argv[1]));
  const std::string outfile = argv[2];
  for (int i = 3; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--window") == 0) {
      rc.proto.recv_window = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    } else if (std::strcmp(argv[i], "--payload") == 0) {
      rc.proto.max_payload = static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
    }
  }
  rc.proto.normalize();

  std::vector<std::uint8_t> data;
  std::fprintf(stderr, "listening on udp/%u ...\n", port);
  const auto res = rudp::net::recv_stream(rudp::net::Address::any(port), &data, rc);
  if (!res.ok) {
    std::fprintf(stderr, "receive failed: %s\n", res.error.c_str());
    return 1;
  }
  FILE* f = std::fopen(outfile.c_str(), "wb");
  if (!f) {
    std::perror("fopen");
    return 1;
  }
  if (!data.empty() && std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
    std::perror("fwrite");
    std::fclose(f);
    return 1;
  }
  std::fclose(f);
  std::printf("received %zu bytes in %.3f s  sha256=%s\n", res.bytes,
              static_cast<double>(res.wall_us) / 1e6, res.sha256.c_str());
  std::printf("  data pkts=%llu dup=%llu out-of-order=%llu acks=%llu bad-cksum=%llu\n",
              static_cast<unsigned long long>(res.rcv.data_packets_received),
              static_cast<unsigned long long>(res.rcv.dup_data),
              static_cast<unsigned long long>(res.rcv.out_of_order),
              static_cast<unsigned long long>(res.rcv.acks_sent),
              static_cast<unsigned long long>(res.rcv.rx_bad_checksum));
  return 0;
}
