// RTT/RTO estimation: RFC 6298 with Karn's algorithm.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "reliable_udp/config.hpp"

namespace rudp {

class RttEstimator {
 public:
  explicit RttEstimator(const Config& cfg)
      : init_rto_us_(cfg.init_rto_us),
        min_rto_us_(cfg.min_rto_us),
        max_rto_us_(cfg.max_rto_us),
        granularity_us_(static_cast<double>(cfg.clock_granularity_us)),
        rto_us_(cfg.init_rto_us) {}

  // A measurement is only ever fed in for a packet that was transmitted
  // exactly once (Karn's algorithm); callers enforce that.
  void on_sample(std::uint64_t rtt_us) {
    if (rtt_us == 0) rtt_us = 1;  // sub-microsecond RTT (loopback / sim)
    const double r = static_cast<double>(rtt_us);
    if (samples_ == 0) {
      srtt_us_ = r;
      rttvar_us_ = r / 2.0;
    } else {
      rttvar_us_ = (1.0 - kBeta) * rttvar_us_ + kBeta * std::fabs(srtt_us_ - r);
      srtt_us_ = (1.0 - kAlpha) * srtt_us_ + kAlpha * r;
    }
    ++samples_;
    sum_us_ += rtt_us;
    min_us_ = std::min(min_us_, rtt_us);
    max_us_ = std::max(max_us_, rtt_us);
    backoff_ = 0;  // a fresh unambiguous sample clears the backoff
    recompute();
  }

  // Retransmission timeout fired: double the RTO (RFC 6298 5.5).
  void on_timeout() {
    if (backoff_ < kMaxBackoff) ++backoff_;
    ++timeouts_;
    recompute();
  }

  std::uint64_t rto_us() const { return rto_us_; }
  std::uint64_t base_rto_us() const { return base_rto_us_; }
  std::uint64_t samples() const { return samples_; }
  std::uint64_t timeouts() const { return timeouts_; }
  std::uint64_t backoff_shift() const { return backoff_; }
  double srtt_us() const { return srtt_us_; }
  double rttvar_us() const { return rttvar_us_; }
  std::uint64_t min_rtt_us() const { return samples_ ? min_us_ : 0; }
  std::uint64_t max_rtt_us() const { return max_us_; }
  double mean_rtt_us() const {
    return samples_ ? static_cast<double>(sum_us_) / static_cast<double>(samples_) : 0.0;
  }

 private:
  void recompute() {
    double base;
    if (samples_ == 0) {
      base = static_cast<double>(init_rto_us_);
    } else {
      base = srtt_us_ + std::max(granularity_us_, 4.0 * rttvar_us_);
    }
    std::uint64_t r = static_cast<std::uint64_t>(base);
    r = std::clamp(r, min_rto_us_, max_rto_us_);
    base_rto_us_ = r;
    // Backoff is applied on top of the clamped base estimate so that a later
    // valid sample restores the estimator immediately.
    const std::uint64_t shifted = (r > (max_rto_us_ >> backoff_)) ? max_rto_us_ : (r << backoff_);
    rto_us_ = std::min(shifted, max_rto_us_);
  }

  static constexpr double kAlpha = 0.125;  // 1/8
  static constexpr double kBeta  = 0.25;   // 1/4
  static constexpr std::uint64_t kMaxBackoff = 6;

  std::uint64_t init_rto_us_;
  std::uint64_t min_rto_us_;
  std::uint64_t max_rto_us_;
  double granularity_us_;

  double srtt_us_ = 0.0;
  double rttvar_us_ = 0.0;
  std::uint64_t samples_ = 0;
  std::uint64_t timeouts_ = 0;
  std::uint64_t sum_us_ = 0;
  std::uint64_t min_us_ = UINT64_MAX;
  std::uint64_t max_us_ = 0;
  std::uint64_t backoff_ = 0;
  std::uint64_t base_rto_us_ = 0;
  std::uint64_t rto_us_;
};

}  // namespace rudp
