#include "test_framework.hpp"

#include <chrono>
#include <cstring>

namespace tf {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

int run_all(int argc, char** argv) {
  const char* filter = nullptr;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];
    else if (std::strcmp(argv[i], "--list") == 0) list = true;
  }
  if (list) {
    for (const auto& t : registry()) std::printf("%s :: %s\n", t.group, t.name);
    return 0;
  }

  int passed = 0, failed = 0, skipped = 0;
  const auto t_start = std::chrono::steady_clock::now();
  for (const auto& t : registry()) {
    if (filter && std::strstr(t.group, filter) == nullptr &&
        std::strstr(t.name, filter) == nullptr) {
      ++skipped;
      continue;
    }
    std::printf("[ RUN      ] %s :: %s\n", t.group, t.name);
    std::fflush(stdout);
    const auto t0 = std::chrono::steady_clock::now();
    try {
      t.fn();
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
      std::printf("[       OK ] %s :: %s (%lld ms)\n", t.group, t.name,
                  static_cast<long long>(ms));
      ++passed;
    } catch (const TestFailure& e) {
      std::printf("[  FAILED  ] %s :: %s\n    %s\n", t.group, t.name, e.what());
      ++failed;
    } catch (const std::exception& e) {
      std::printf("[  FAILED  ] %s :: %s\n    unexpected exception: %s\n", t.group, t.name,
                  e.what());
      ++failed;
    }
    std::fflush(stdout);
  }
  const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t_start).count();
  std::printf("\n%d passed, %d failed, %d filtered out (%lld ms)\n", passed, failed, skipped,
              static_cast<long long>(total_ms));
  return failed == 0 ? 0 : 1;
}

}  // namespace tf

int main(int argc, char** argv) { return tf::run_all(argc, argv); }
