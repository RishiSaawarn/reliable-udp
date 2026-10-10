#include "reliable_udp/net/server.hpp"

#include <algorithm>
#include <ctime>

#include "reliable_udp/packet.hpp"

namespace rudp::net {

Server::Server(const Address& bind_addr, const ServerConfig& cfg) : cfg_(cfg) {
  cfg_.proto.normalize();
  sock_.bind_to(bind_addr);
  sock_.set_nonblocking(true);
  sock_.set_rcvbuf(cfg_.rcvbuf_bytes);
  sock_.set_sndbuf(cfg_.sndbuf_bytes);
}

Server::~Server() { stop(); }

void Server::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;
  rx_thread_ = std::thread([this] { rx_loop(); });
  timer_thread_ = std::thread([this] { timer_loop(); });
}

void Server::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) return;
  if (rx_thread_.joinable()) rx_thread_.join();
  if (timer_thread_.joinable()) timer_thread_.join();
}

std::size_t Server::active_sessions() const {
  std::shared_lock<std::shared_mutex> lk(table_mu_);
  return table_.size();
}

std::vector<SessionResult> Server::take_completed() {
  std::lock_guard<std::mutex> lk(done_mu_);
  std::vector<SessionResult> out;
  out.swap(done_);
  return out;
}

void Server::finish(const std::shared_ptr<Session>& s, bool ok) {
  SessionResult r;
  r.session_id = s->session_id;
  r.peer = s->peer.str();
  r.bytes = s->bytes;
  r.sha256 = to_hex(s->hasher->finish());
  r.stats = s->rx->stats();
  r.wall_us = now_us() - s->started_us;
  r.ok = ok;
  if (cfg_.keep_payload) r.payload = std::move(s->payload);
  {
    std::lock_guard<std::mutex> lk(done_mu_);
    done_.push_back(std::move(r));
  }
  completed_count_.fetch_add(1, std::memory_order_relaxed);
}

void Server::rx_loop() {
  std::vector<std::uint8_t> buf(kMaxDatagramSize);
  while (running_.load(std::memory_order_relaxed)) {
    if (!sock_.wait_readable(2000)) continue;
    for (std::uint32_t i = 0; i < cfg_.max_burst_reads; ++i) {
      Address from;
      std::ptrdiff_t n;
      try {
        n = sock_.recv_from(buf.data(), buf.size(), &from);
      } catch (const SocketError&) {
        break;
      }
      if (n < 0) break;
      rx_datagrams_.fetch_add(1, std::memory_order_relaxed);

      // Peek the header only to route the datagram; the session's own engine
      // re-parses and fully validates it (checksum included).
      if (static_cast<std::size_t>(n) < kHeaderSize) continue;
      const std::uint8_t version = buf[0];
      const std::uint8_t type = buf[1];
      if (version != kVersion || !is_valid_type(type)) continue;
      const std::uint32_t sid = (static_cast<std::uint32_t>(buf[4]) << 24) |
                                (static_cast<std::uint32_t>(buf[5]) << 16) |
                                (static_cast<std::uint32_t>(buf[6]) << 8) |
                                static_cast<std::uint32_t>(buf[7]);
      const Key key{from.key(), sid};

      std::shared_ptr<Session> s;
      {
        std::shared_lock<std::shared_mutex> lk(table_mu_);
        auto it = table_.find(key);
        if (it != table_.end()) s = it->second;
      }
      if (!s) {
        if (type != static_cast<std::uint8_t>(PacketType::Syn)) {
          // Unknown session: tell the peer instead of letting it time out.
          rx_unknown_.fetch_add(1, std::memory_order_relaxed);
          Header h;
          h.type = PacketType::Rst;
          h.session_id = sid;
          std::uint8_t rst[kMaxOverhead];
          const std::size_t len = encode_packet(rst, h, nullptr, 0, nullptr, 0);
          sock_.send_gather(rst, len, nullptr, 0, &from);
          continue;
        }
        s = std::make_shared<Session>();
        s->peer = from;
        s->session_id = sid;
        s->started_us = now_us();
        s->last_activity_us = s->started_us;
        s->hasher = std::make_unique<Sha256>();
        s->sink = std::make_unique<UdpSink>(sock_, from);
        Session* raw = s.get();
        const bool keep = cfg_.keep_payload;
        s->rx = std::make_unique<SrReceiver>(
            cfg_.proto, *s->sink, [raw, keep](const std::uint8_t* p, std::size_t len) {
              raw->hasher->update(p, len);
              raw->bytes += len;
              if (keep) raw->payload.insert(raw->payload.end(), p, p + len);
            });
        {
          std::unique_lock<std::shared_mutex> lk(table_mu_);
          auto ins = table_.emplace(key, s);
          if (!ins.second) s = ins.first->second;  // lost a race, use the winner
        }
      }

      {
        std::lock_guard<std::mutex> lk(s->mu);
        s->rx->on_datagram(buf.data(), static_cast<std::size_t>(n), now_us());
        s->last_activity_us = now_us();
      }
    }
  }
}

void Server::timer_loop() {
  std::vector<std::pair<Key, std::shared_ptr<Session>>> snapshot;
  while (running_.load(std::memory_order_relaxed)) {
    snapshot.clear();
    {
      std::shared_lock<std::shared_mutex> lk(table_mu_);
      snapshot.assign(table_.begin(), table_.end());
    }
    std::vector<Key> reap;
    for (auto& [key, s] : snapshot) {
      bool done = false;
      bool timed_out = false;
      {
        std::lock_guard<std::mutex> lk(s->mu);
        const std::uint64_t now = now_us();
        s->rx->tick(now);
        done = s->rx->done();
        timed_out = !done && (now - s->last_activity_us > cfg_.session_idle_timeout_us);
        if ((done || timed_out) && !s->reaped) {
          s->reaped = true;
        } else {
          done = false;
          timed_out = false;
        }
      }
      if (done || timed_out) {
        finish(s, done);
        reap.push_back(key);
      }
    }
    if (!reap.empty()) {
      std::unique_lock<std::shared_mutex> lk(table_mu_);
      for (const Key& k : reap) table_.erase(k);
    }
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(cfg_.tick_interval_us / 1000000ull);
    ts.tv_nsec = static_cast<long>((cfg_.tick_interval_us % 1000000ull) * 1000ull);
    ::nanosleep(&ts, nullptr);
  }

  // Drain anything still open at shutdown so callers see a complete picture.
  std::vector<std::shared_ptr<Session>> leftovers;
  {
    std::unique_lock<std::shared_mutex> lk(table_mu_);
    for (auto& [key, s] : table_) leftovers.push_back(s);
    table_.clear();
  }
  for (auto& s : leftovers) {
    bool report = false;
    {
      std::lock_guard<std::mutex> lk(s->mu);
      if (!s->reaped) {
        s->reaped = true;
        report = true;
      }
    }
    if (report) finish(s, false);
  }
}

}  // namespace rudp::net
