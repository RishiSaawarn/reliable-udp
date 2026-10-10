#include "bench_common.hpp"

#include <sys/resource.h>
#include <sys/utsname.h>
#include <ctime>

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>

namespace bench {
namespace {

std::string trim(std::string s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string read_first_line(const char* path) {
  std::ifstream f(path);
  std::string line;
  if (f && std::getline(f, line)) return trim(line);
  return "";
}

std::string proc_cpuinfo_field(const char* key) {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (f && std::getline(f, line)) {
    if (line.rfind(key, 0) == 0) {
      const auto colon = line.find(':');
      if (colon != std::string::npos) return trim(line.substr(colon + 1));
    }
  }
  return "unknown";
}

std::size_t count_cpus() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  std::size_t n = 0;
  while (f && std::getline(f, line)) {
    if (line.rfind("processor", 0) == 0) ++n;
  }
  return n;
}

std::string os_pretty_name() {
  std::ifstream f("/etc/os-release");
  std::string line;
  while (f && std::getline(f, line)) {
    if (line.rfind("PRETTY_NAME=", 0) == 0) {
      std::string v = line.substr(12);
      if (!v.empty() && v.front() == '"') v = v.substr(1, v.size() - 2);
      return v;
    }
  }
  return "unknown";
}

}  // namespace

std::string fmt(const char* format, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, format);
  std::vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  return std::string(buf);
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

Env capture_env() {
  Env e;
  e.cpu_model = proc_cpuinfo_field("model name");
  const std::size_t cpus = count_cpus();
  const std::string siblings = proc_cpuinfo_field("siblings");
  const std::string cores = proc_cpuinfo_field("cpu cores");
  e.cpu_topology = fmt("%zu logical CPUs (%s cores/socket, %s siblings/socket)", cpus,
                       cores.c_str(), siblings.c_str());
  const std::string mhz = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
  e.cpu_mhz_max = mhz.empty() ? "unknown" : fmt("%.0f MHz", std::atof(mhz.c_str()) / 1000.0);
  e.governor = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  if (e.governor.empty()) e.governor = "unknown";

  utsname u{};
  if (::uname(&u) == 0) e.kernel = fmt("%s %s %s", u.sysname, u.release, u.machine);
  else e.kernel = "unknown";
  e.os = os_pretty_name();

#if defined(__clang__)
  e.compiler = fmt("clang++ %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
  e.compiler = fmt("g++ %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
  e.compiler = "unknown";
#endif
  e.cxx_standard = fmt("__cplusplus=%ld", static_cast<long>(__cplusplus));
#ifdef RUDP_BUILD_TYPE
  e.build_type = RUDP_BUILD_TYPE;
#else
  e.build_type = "unknown";
#endif
#ifdef RUDP_CXX_FLAGS
  e.opt_flags = RUDP_CXX_FLAGS;
#else
  e.opt_flags = "unknown";
#endif

  time_t t = ::time(nullptr);
  tm g{};
  ::gmtime_r(&t, &g);
  char ts[64];
  ::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &g);
  e.timestamp_utc = ts;
  return e;
}

void print_env(const Env& e) {
  std::printf("=== environment ===============================================================\n");
  std::printf("  timestamp    : %s\n", e.timestamp_utc.c_str());
  std::printf("  cpu          : %s\n", e.cpu_model.c_str());
  std::printf("  topology     : %s\n", e.cpu_topology.c_str());
  std::printf("  max freq     : %s   scaling governor: %s\n", e.cpu_mhz_max.c_str(),
              e.governor.c_str());
  std::printf("  os           : %s\n", e.os.c_str());
  std::printf("  kernel       : %s\n", e.kernel.c_str());
  std::printf("  compiler     : %s   (%s)\n", e.compiler.c_str(), e.cxx_standard.c_str());
  std::printf("  build type   : %s\n", e.build_type.c_str());
  std::printf("  flags        : %s\n", e.opt_flags.c_str());
  std::printf("===============================================================================\n\n");
}

std::string env_csv_header() {
  return "timestamp_utc,cpu_model,cpu_topology,cpu_max_freq,governor,os,kernel,compiler,"
         "cxx_standard,build_type,opt_flags";
}

std::string env_csv_row(const Env& e) {
  auto q = [](const std::string& s) { return "\"" + s + "\""; };
  return join({q(e.timestamp_utc), q(e.cpu_model), q(e.cpu_topology), q(e.cpu_mhz_max),
               q(e.governor), q(e.os), q(e.kernel), q(e.compiler), q(e.cxx_standard),
               q(e.build_type), q(e.opt_flags)},
              ",");
}

CpuTime cpu_time() {
  rusage r{};
  ::getrusage(RUSAGE_SELF, &r);
  CpuTime c;
  c.user_us = static_cast<std::uint64_t>(r.ru_utime.tv_sec) * 1000000ull +
              static_cast<std::uint64_t>(r.ru_utime.tv_usec);
  c.sys_us = static_cast<std::uint64_t>(r.ru_stime.tv_sec) * 1000000ull +
             static_cast<std::uint64_t>(r.ru_stime.tv_usec);
  return c;
}

CpuTime cpu_delta(const CpuTime& a, const CpuTime& b) {
  CpuTime c;
  c.user_us = b.user_us - a.user_us;
  c.sys_us = b.sys_us - a.sys_us;
  return c;
}

std::uint64_t wall_us() {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ull +
         static_cast<std::uint64_t>(ts.tv_nsec) / 1000ull;
}

Dist summarise(std::vector<double> v) {
  Dist d;
  if (v.empty()) return d;
  std::sort(v.begin(), v.end());
  d.n = v.size();
  d.min = v.front();
  d.max = v.back();
  auto pct = [&](double p) {
    const double idx = p * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
  };
  d.p25 = pct(0.25);
  d.median = pct(0.50);
  d.p75 = pct(0.75);
  d.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
  double acc = 0;
  for (double x : v) acc += (x - d.mean) * (x - d.mean);
  d.stddev = v.size() > 1 ? std::sqrt(acc / static_cast<double>(v.size() - 1)) : 0.0;
  return d;
}

std::string Dist::str() const {
  return fmt("median=%.4g  [min %.4g | p25 %.4g | p75 %.4g | max %.4g]  mean=%.4g sd=%.4g n=%zu",
             median, min, p25, p75, max, mean, stddev, n);
}

Csv::Csv(const std::string& path) : path_(path) {
  f_ = std::fopen(path.c_str(), "w");
  if (!f_) std::fprintf(stderr, "warning: cannot open %s for writing\n", path.c_str());
}
Csv::~Csv() {
  if (f_) std::fclose(static_cast<FILE*>(f_));
}
void Csv::header(const std::string& cols) {
  if (f_) std::fprintf(static_cast<FILE*>(f_), "%s\n", cols.c_str());
}
void Csv::row(const std::string& values) {
  if (f_) {
    std::fprintf(static_cast<FILE*>(f_), "%s\n", values.c_str());
    std::fflush(static_cast<FILE*>(f_));
  }
}

}  // namespace bench
