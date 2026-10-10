// Benchmark support: environment capture, repetition statistics, CSV output.
//
// Every benchmark records the full environment and the full parameter set, so
// a number can always be traced back to the exact conditions that produced it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bench {

struct Env {
  std::string cpu_model;
  std::string cpu_topology;
  std::string cpu_mhz_max;
  std::string os;
  std::string kernel;
  std::string compiler;
  std::string cxx_standard;
  std::string build_type;
  std::string opt_flags;
  std::string timestamp_utc;
  std::string governor;
};

Env capture_env();
void print_env(const Env& e);
std::string env_csv_header();
std::string env_csv_row(const Env& e);

// Process-wide CPU time (user + system), microseconds.
struct CpuTime {
  std::uint64_t user_us = 0;
  std::uint64_t sys_us = 0;
  std::uint64_t total_us() const { return user_us + sys_us; }
};
CpuTime cpu_time();
CpuTime cpu_delta(const CpuTime& before, const CpuTime& after);

std::uint64_t wall_us();

// Distribution summary over repetitions. Median is the headline figure; the
// spread is always reported alongside so a single lucky run cannot be quoted.
struct Dist {
  std::size_t n = 0;
  double min = 0, p25 = 0, median = 0, p75 = 0, max = 0, mean = 0, stddev = 0;
  std::string str() const;
};
Dist summarise(std::vector<double> v);

class Csv {
 public:
  explicit Csv(const std::string& path);
  ~Csv();
  Csv(const Csv&) = delete;
  Csv& operator=(const Csv&) = delete;
  void header(const std::string& cols);
  void row(const std::string& values);
  bool ok() const { return f_ != nullptr; }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  void* f_ = nullptr;
};

std::string fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string join(const std::vector<std::string>& parts, const char* sep);

// Megabits per second from a byte count and a microsecond duration.
inline double mbps(std::size_t bytes, std::uint64_t us) {
  if (us == 0) return 0.0;
  return static_cast<double>(bytes) * 8.0 / static_cast<double>(us);
}

}  // namespace bench
