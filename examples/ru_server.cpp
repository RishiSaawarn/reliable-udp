// Multi-session receiver: accepts N concurrent transfers on one UDP port.
//   ru_server <port> [--sessions N] [--keep]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "reliable_udp/net/server.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <port> [--sessions N]\n", argv[0]);
    return 2;
  }
  const auto port = static_cast<std::uint16_t>(std::atoi(argv[1]));
  int expect = 1;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--sessions") == 0) expect = std::atoi(argv[i + 1]);
  }

  rudp::net::ServerConfig cfg;
  rudp::net::Server server(rudp::net::Address::any(port), cfg);
  server.start();
  std::fprintf(stderr, "server listening on %s, waiting for %d session(s)\n",
               server.local_address().str().c_str(), expect);

  int seen = 0;
  while (seen < expect) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (const auto& r : server.take_completed()) {
      ++seen;
      std::printf("session 0x%08x from %s: %s %zu bytes in %.3f s sha256=%s "
                  "(dup=%llu ooo=%llu acks=%llu)\n",
                  r.session_id, r.peer.c_str(), r.ok ? "OK" : "FAILED", r.bytes,
                  static_cast<double>(r.wall_us) / 1e6, r.sha256.c_str(),
                  static_cast<unsigned long long>(r.stats.dup_data),
                  static_cast<unsigned long long>(r.stats.out_of_order),
                  static_cast<unsigned long long>(r.stats.acks_sent));
      std::fflush(stdout);
    }
  }
  server.stop();
  return 0;
}
