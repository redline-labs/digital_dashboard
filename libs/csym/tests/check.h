#pragma once

// Minimal test harness: no dependencies, one executable per test file.

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace check {
inline int failures = 0;
inline int checks = 0;

inline bool close(double a, double b, double rtol, double atol) {
  if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
  return std::fabs(a - b) <= atol + rtol * std::fabs(b);
}

inline void near(double a, double b, double rtol, double atol, const char* what, const char* file, int line) {
  ++checks;
  if (!close(a, b, rtol, atol)) {
    ++failures;
    std::printf("%s:%d: FAIL %s: got %.17g expected %.17g (diff %.3g)\n", file, line, what, a, b, a - b);
  }
}

inline int report(const char* name) {
  std::printf("%s: %d checks, %d failures\n", name, checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
}  // namespace check

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++check::checks;                                                             \
    if (!(cond)) {                                                               \
      ++check::failures;                                                         \
      std::printf("%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                            \
  } while (0)
#define CHECK_NEAR(a, b, tol) check::near((a), (b), (tol), (tol), #a, __FILE__, __LINE__)
#define CHECK_CLOSE(a, b, rtol, atol) check::near((a), (b), (rtol), (atol), #a, __FILE__, __LINE__)
