// One complete loopback transfer over real UDP sockets: receiver on its own
// thread, sender on the calling thread. Used by the wall-clock benchmarks.
#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "reliable_udp/net/session_runner.hpp"

namespace bench {

struct LoopbackResult {
  bool ok = false;
  std::string error;
  std::uint64_t wall_us = 0;       // measured on the sender
  std::uint64_t recv_wall_us = 0;
  std::size_t bytes = 0;
  bool sha_ok = false;
  rudp::net::RunResult sender;
  rudp::net::RunResult receiver;
  CpuTime cpu;
  // Datagrams the sender put on the wire that the receiver never saw. On
  // loopback this is almost entirely socket-receive-buffer overflow.
  std::uint64_t missing_datagrams = 0;
};

inline LoopbackResult run_loopback(const std::vector<std::uint8_t>& data,
                                   const rudp::net::RunnerConfig& snd_cfg,
                                   const rudp::net::RunnerConfig& rcv_cfg,
                                   bool verify_bytes = true) {
  using namespace rudp::net;
  LoopbackResult r;
  UdpSocket rx;
  rx.bind_to(Address::loopback(0));
  const Address peer = rx.local_address();

  std::vector<std::uint8_t> got;
  if (verify_bytes) got.reserve(data.size());
  RunResult rres;
  std::thread rt([&] { rres = recv_stream_on(rx, verify_bytes ? &got : nullptr, rcv_cfg); });
  // Let the receive thread reach its first ppoll before the SYN goes out.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  const CpuTime c0 = cpu_time();
  const std::uint64_t t0 = wall_us();
  RunResult sres = send_stream(peer, data.empty() ? nullptr : data.data(), data.size(), snd_cfg);
  const std::uint64_t t1 = wall_us();
  const CpuTime c1 = cpu_time();
  rt.join();

  r.sender = sres;
  r.receiver = rres;
  r.wall_us = t1 - t0;
  r.recv_wall_us = rres.wall_us;
  r.bytes = data.size();
  r.cpu = cpu_delta(c0, c1);
  r.ok = sres.ok && rres.ok;
  if (!sres.ok) r.error = "send: " + sres.error;
  else if (!rres.ok) r.error = "recv: " + rres.error;
  r.sha_ok = sres.sha256 == rres.sha256 && rres.bytes == data.size();
  if (verify_bytes && r.sha_ok) r.sha_ok = (got == data);
  const std::uint64_t sent = sres.tx_datagrams;
  r.missing_datagrams = sent > rres.rx_datagrams ? sent - rres.rx_datagrams : 0;
  return r;
}

}  // namespace bench
