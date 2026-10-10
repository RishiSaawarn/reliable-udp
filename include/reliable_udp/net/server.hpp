// Multi-session receiver: one bound UDP socket, many concurrent transfers.
//
// Threading model (deliberately a *real* shared-state design, so that TSan has
// something to check):
//   * one receive thread   - drains the socket, demultiplexes by
//                            (peer address, session id), creates sessions
//   * one timer thread     - walks the session table and drives retransmission
//                            / delayed-ACK / linger timers
// The session table is guarded by a std::shared_mutex (shared for the
// per-datagram lookup, exclusive for create/reap). Each Session carries its own
// std::mutex; every call into that session's SrReceiver -- from either thread --
// happens under it, so an individual protocol state machine is still driven
// single-threaded even though two OS threads reach it.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "reliable_udp/config.hpp"
#include "reliable_udp/net/udp_socket.hpp"
#include "reliable_udp/receiver.hpp"
#include "reliable_udp/sha256.hpp"

namespace rudp::net {

struct ServerConfig {
  Config proto{};
  int rcvbuf_bytes = 16 << 20;
  int sndbuf_bytes = 16 << 20;
  std::uint64_t tick_interval_us = 500;
  std::uint64_t session_idle_timeout_us = 30ull * 1000 * 1000;
  bool keep_payload = false;  // false: hash only, do not retain the bytes
  std::uint32_t max_burst_reads = 256;
};

struct SessionResult {
  std::uint32_t session_id = 0;
  std::string peer;
  std::size_t bytes = 0;
  std::string sha256;
  ReceiverStats stats{};
  std::uint64_t wall_us = 0;
  bool ok = false;
  std::vector<std::uint8_t> payload;  // only when keep_payload
};

class Server {
 public:
  Server(const Address& bind_addr, const ServerConfig& cfg);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  void start();
  void stop();

  Address local_address() const { return sock_.local_address(); }
  std::size_t active_sessions() const;
  std::uint64_t completed_count() const { return completed_count_.load(std::memory_order_relaxed); }
  std::uint64_t rx_datagrams() const { return rx_datagrams_.load(std::memory_order_relaxed); }
  std::uint64_t rx_unknown_session() const {
    return rx_unknown_.load(std::memory_order_relaxed);
  }
  std::vector<SessionResult> take_completed();

 private:
  struct Session {
    std::mutex mu;
    std::unique_ptr<UdpSink> sink;
    std::unique_ptr<SrReceiver> rx;
    std::unique_ptr<Sha256> hasher;
    Address peer;
    std::uint32_t session_id = 0;
    std::size_t bytes = 0;
    std::vector<std::uint8_t> payload;
    std::uint64_t started_us = 0;
    std::uint64_t last_activity_us = 0;
    bool reaped = false;
  };
  using Key = std::pair<std::uint64_t, std::uint32_t>;

  void rx_loop();
  void timer_loop();
  void finish(const std::shared_ptr<Session>& s, bool ok);

  ServerConfig cfg_;
  UdpSocket sock_;
  std::atomic<bool> running_{false};
  std::thread rx_thread_;
  std::thread timer_thread_;

  mutable std::shared_mutex table_mu_;
  std::map<Key, std::shared_ptr<Session>> table_;

  std::mutex done_mu_;
  std::vector<SessionResult> done_;

  std::atomic<std::uint64_t> completed_count_{0};
  std::atomic<std::uint64_t> rx_datagrams_{0};
  std::atomic<std::uint64_t> rx_unknown_{0};
};

}  // namespace rudp::net
