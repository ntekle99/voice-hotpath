// Minimal assertion harness. No gtest dependency: this project should build on
// a bare box with nothing but a compiler and CMake.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace vhptest {

inline int g_failures = 0;
inline int g_checks = 0;

inline void check(bool condition, const char* expr, const char* file, int line,
                  const std::string& detail = {}) {
  ++g_checks;
  if (condition) return;
  ++g_failures;
  std::fprintf(stderr, "FAIL %s:%d  %s%s%s\n", file, line, expr,
               detail.empty() ? "" : "  --  ", detail.c_str());
}

inline int summarize(const char* name) {
  std::fprintf(stderr, "%s: %d checks, %d failures\n", name, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace vhptest

#define CHECK(cond) ::vhptest::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_MSG(cond, detail) ::vhptest::check((cond), #cond, __FILE__, __LINE__, (detail))
#define CHECK_EQ(a, b)                                                            \
  ::vhptest::check((a) == (b), #a " == " #b, __FILE__, __LINE__,                  \
                   std::to_string(a) + " vs " + std::to_string(b))
#define CHECK_NEAR(a, b, tol)                                                     \
  ::vhptest::check(std::fabs((a) - (b)) <= (tol), #a " ~= " #b, __FILE__, __LINE__, \
                   std::to_string(a) + " vs " + std::to_string(b))
