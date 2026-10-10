// Minimal self-contained test registry. No external dependency, so the whole
// suite builds under every sanitizer configuration without extra plumbing.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace tf {

struct TestFailure : std::exception {
  std::string msg;
  explicit TestFailure(std::string m) : msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};

struct TestCase {
  const char* group;
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry();
int run_all(int argc, char** argv);

struct Registrar {
  Registrar(const char* group, const char* name, void (*fn)()) {
    registry().push_back({group, name, fn});
  }
};

}  // namespace tf

#define RUDP_CONCAT_(a, b) a##b
#define RUDP_CONCAT(a, b) RUDP_CONCAT_(a, b)

#define TEST(group_, name_)                                                             \
  static void RUDP_CONCAT(rudp_test_, __LINE__)();                                      \
  static ::tf::Registrar RUDP_CONCAT(rudp_reg_, __LINE__)(#group_, name_,               \
                                                          &RUDP_CONCAT(rudp_test_,      \
                                                                       __LINE__));      \
  static void RUDP_CONCAT(rudp_test_, __LINE__)()

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::ostringstream oss_;                                                          \
      oss_ << __FILE__ << ":" << __LINE__ << ": CHECK failed: " << #cond;               \
      throw ::tf::TestFailure(oss_.str());                                              \
    }                                                                                   \
  } while (0)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    const auto va_ = (a);                                                               \
    const auto vb_ = (b);                                                               \
    if (!(va_ == vb_)) {                                                                \
      std::ostringstream oss_;                                                          \
      oss_ << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " << #a << " == " << #b \
           << "  (" << va_ << " vs " << vb_ << ")";                                     \
      throw ::tf::TestFailure(oss_.str());                                              \
    }                                                                                   \
  } while (0)

#define CHECK_MSG(cond, msg)                                                            \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::ostringstream oss_;                                                          \
      oss_ << __FILE__ << ":" << __LINE__ << ": " << (msg);                             \
      throw ::tf::TestFailure(oss_.str());                                              \
    }                                                                                   \
  } while (0)
