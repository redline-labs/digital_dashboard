#pragma once

// constexpr implementations of the elementary functions, plus scalar front-ends that use them during
// constant evaluation and <cmath> at runtime.
//
// The constexpr versions are accurate to a few ulp over the ranges that matter for constant folding and
// compile-time testing. They are not correctly rounded, so compare against std:: results with a tolerance.

#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>

#include "csym/config.h"

namespace csym::cm {

inline constexpr double pi = 3.141592653589793238462643383279502884;
inline constexpr double pi_2 = 1.570796326794896619231321691639751442;
inline constexpr double ln2_hi = 6.93147180369123816490e-01;
inline constexpr double ln2_lo = 1.90821492927058770002e-10;
inline constexpr double ln2 = 0.693147180559945309417232121458176568;
inline constexpr double inf = std::numeric_limits<double>::infinity();
inline constexpr double nan = std::numeric_limits<double>::quiet_NaN();

constexpr bool isnan(double x) { return x != x; }
constexpr bool isinf(double x) { return x == inf || x == -inf; }
constexpr bool signbit(double x) { return (std::bit_cast<std::uint64_t>(x) >> 63) != 0; }
constexpr double fabs(double x) { return signbit(x) ? -x : x; }
constexpr double copysign(double mag, double sgn) { return signbit(sgn) != signbit(mag) ? -mag : mag; }

// x * 2^e
constexpr double ldexp(double x, int e) {
  while (e > 1000) { x *= 0x1p1000; e -= 1000; }
  while (e < -1000) { x *= 0x1p-1000; e += 1000; }
  return x * std::bit_cast<double>(static_cast<std::uint64_t>(e + 1023) << 52);
}

// x = m * 2^e with m in [0.5, 1). x must be finite and nonzero.
constexpr double frexp(double x, int* e) {
  int adj = 0;
  if (fabs(x) < 0x1p-1000) { x *= 0x1p100; adj = -100; }
  auto bits = std::bit_cast<std::uint64_t>(x);
  const int be = static_cast<int>((bits >> 52) & 0x7ff);
  *e = be - 1022 + adj;
  bits = (bits & ~(std::uint64_t{0x7ff} << 52)) | (std::uint64_t{1022} << 52);
  return std::bit_cast<double>(bits);
}

constexpr double trunc(double x) {
  if (isnan(x) || isinf(x) || fabs(x) >= 0x1p52) return x;
  return static_cast<double>(static_cast<std::int64_t>(x));
}
constexpr double floor(double x) {
  const double t = trunc(x);
  return (t > x) ? t - 1.0 : t;
}
constexpr double round(double x) {  // half away from zero
  return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);
}
constexpr bool is_integer(double x) { return !isnan(x) && !isinf(x) && trunc(x) == x; }

constexpr double sqrt(double x) {
  if (isnan(x) || x < 0) return nan;
  if (x == 0 || x == inf) return x;
  int e = 0;
  double m = frexp(x, &e);
  if (e & 1) { m *= 2.0; e -= 1; }  // m in [0.5, 2), e even
  double y = 0.5 * (1.0 + m);
  for (int i = 0; i < 7; ++i) y = 0.5 * (y + m / y);
  return ldexp(y, e / 2);
}

constexpr double exp(double x) {
  if (isnan(x)) return x;
  if (x > 709.782712893384) return inf;
  if (x < -745.1332191019412) return 0.0;
  const double kf = round(x / ln2);
  const double r = (x - kf * ln2_hi) - kf * ln2_lo;  // |r| <= ln2/2
  double term = 1.0, sum = 1.0;
  for (int i = 1; i <= 22; ++i) { term *= r / i; sum += term; }
  return ldexp(sum, static_cast<int>(kf));
}

constexpr double expm1(double x) {
  if (fabs(x) < 0.5) {
    double term = x, sum = x;
    for (int i = 2; i <= 22; ++i) { term *= x / i; sum += term; }
    return sum;
  }
  return exp(x) - 1.0;
}

constexpr double log(double x) {
  if (isnan(x) || x < 0) return nan;
  if (x == 0) return -inf;
  if (x == inf) return x;
  int e = 0;
  double m = frexp(x, &e);
  if (m < 0.70710678118654752440) { m *= 2.0; e -= 1; }  // m in [sqrt(.5), sqrt(2))
  const double s = (m - 1.0) / (m + 1.0), s2 = s * s;
  double term = s, sum = 0.0;
  for (int k = 0; k < 24; ++k) { sum += term / (2 * k + 1); term *= s2; }
  return 2.0 * sum + e * ln2_lo + e * ln2_hi;
}

// Cody-Waite reduction x = k*(pi/2) + r with |r| <= pi/4 (constants from fdlibm's rem_pio2).
constexpr double reduce_pio2(double x, int* quadrant) {
  const double k = round(x / pi_2);
  const double r = ((x - k * 1.57079632673412561417e+00) - k * 6.07710050630396597660e-11) -
                   k * 2.02226624871116645580e-21;
  *quadrant = static_cast<int>(static_cast<std::int64_t>(k) & 3);
  return r;
}
constexpr double sin_poly(double r) {
  const double r2 = r * r;
  double term = r, sum = r;
  for (int i = 1; i < 14; ++i) { term *= -r2 / ((2 * i) * (2 * i + 1)); sum += term; }
  return sum;
}
constexpr double cos_poly(double r) {
  const double r2 = r * r;
  double term = 1.0, sum = 1.0;
  for (int i = 1; i < 14; ++i) { term *= -r2 / ((2 * i - 1) * (2 * i)); sum += term; }
  return sum;
}
constexpr double sin(double x) {
  if (isnan(x) || isinf(x)) return nan;
  if (x == 0) return x;
  int q = 0;
  const double r = reduce_pio2(x, &q);
  switch (q) {
    case 0: return sin_poly(r);
    case 1: return cos_poly(r);
    case 2: return -sin_poly(r);
    default: return -cos_poly(r);
  }
}
constexpr double cos(double x) {
  if (isnan(x) || isinf(x)) return nan;
  int q = 0;
  const double r = reduce_pio2(x, &q);
  switch (q) {
    case 0: return cos_poly(r);
    case 1: return -sin_poly(r);
    case 2: return -cos_poly(r);
    default: return sin_poly(r);
  }
}
constexpr double tan(double x) { return sin(x) / cos(x); }

constexpr double atan(double x) {
  if (isnan(x)) return x;
  if (x < 0) return -atan(-x);
  if (x == inf) return pi_2;
  bool invert = false;
  if (x > 1.0) { x = 1.0 / x; invert = true; }
  double offset = 0.0;
  constexpr double tan_pi_12 = 0.26794919243112270647;
  constexpr double inv_sqrt3 = 0.57735026918962576451;
  if (x > tan_pi_12) {  // atan(x) = pi/6 + atan((x - 1/sqrt3) / (1 + x/sqrt3))
    x = (x - inv_sqrt3) / (1.0 + x * inv_sqrt3);
    offset = pi / 6.0;
  }
  const double x2 = x * x;
  double term = x, sum = 0.0;
  for (int k = 0; k < 18; ++k) { sum += term / (2 * k + 1); term *= -x2; }
  const double r = offset + sum;
  return invert ? pi_2 - r : r;
}

constexpr double atan2(double y, double x) {
  if (isnan(x) || isnan(y)) return nan;
  if (y == 0) {
    if (signbit(x)) return copysign(pi, y);
    return y;  // +-0
  }
  if (x == 0) return copysign(pi_2, y);
  if (isinf(x) && isinf(y)) return copysign(x > 0 ? pi / 4 : 3 * pi / 4, y);
  const double a = atan(fabs(y / x));
  if (x > 0) return copysign(a, y);
  return copysign(pi - a, y);
}

constexpr double asin(double x) {
  if (isnan(x) || fabs(x) > 1.0) return nan;
  return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}
constexpr double acos(double x) {
  if (isnan(x) || fabs(x) > 1.0) return nan;
  return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

constexpr double tanh(double x) {
  if (isnan(x)) return x;
  const double ax = fabs(x);
  if (ax > 22.0) return copysign(1.0, x);
  const double em = expm1(2.0 * ax);
  return copysign(em / (em + 2.0), x);
}

constexpr double pow(double x, double y) {
  if (y == 0) return 1.0;
  if (isnan(x) || isnan(y)) return nan;
  if (is_integer(y) && fabs(y) < 0x1p31) {
    auto n = static_cast<std::int64_t>(fabs(y));
    double base = x, result = 1.0;
    while (n) {
      if (n & 1) result *= base;
      base *= base;
      n >>= 1;
    }
    return y < 0 ? 1.0 / result : result;
  }
  if (x < 0) return nan;
  if (x == 0) return y > 0 ? 0.0 : inf;
  return exp(y * log(x));
}

constexpr double sign(double x) { return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); }
constexpr double sign_no_zero(double x) { return x < 0 ? -1.0 : 1.0; }
constexpr double fmin(double a, double b) { return b < a ? b : a; }
constexpr double fmax(double a, double b) { return a < b ? b : a; }

}  // namespace csym::cm

namespace csym {

// Scalar front-ends: constexpr (csym::cm) during constant evaluation, <cmath> at runtime. These are also the
// overloads generic user code reaches for plain floating-point types, so the same lambda can be traced
// symbolically or run numerically. They are force-inlined: inside a large generated evaluator GCC stops
// inlining ordinary functions (its function-growth limits), which would turn each of these into a real
// call and prevent fusing sin/cos pairs.
#define CSYM_UNARY_MATH(name, stdname)                                        \
  template <std::floating_point T>                                           \
  CSYM_ALWAYS_INLINE constexpr T name(T x) {                                 \
    if consteval {                                                           \
      return static_cast<T>(cm::name(static_cast<double>(x)));              \
    } else {                                                                 \
      return std::stdname(x);                                                \
    }                                                                        \
  }
CSYM_UNARY_MATH(sin, sin)
CSYM_UNARY_MATH(cos, cos)
CSYM_UNARY_MATH(tan, tan)
CSYM_UNARY_MATH(asin, asin)
CSYM_UNARY_MATH(acos, acos)
CSYM_UNARY_MATH(atan, atan)
CSYM_UNARY_MATH(exp, exp)
CSYM_UNARY_MATH(log, log)
CSYM_UNARY_MATH(tanh, tanh)
CSYM_UNARY_MATH(sqrt, sqrt)
CSYM_UNARY_MATH(floor, floor)
#undef CSYM_UNARY_MATH

template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T abs(T x) { return x < 0 ? -x : x; }
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T sign(T x) { return x > 0 ? T(1) : (x < 0 ? T(-1) : T(0)); }
// sign(x), except sign_no_zero(0) == 1.
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T sign_no_zero(T x) { return x < 0 ? T(-1) : T(1); }
// |a| with the sign of b, where b == 0 counts as positive.
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T copysign_no_zero(T a, T b) { return abs(a) * sign_no_zero(b); }
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T min(T a, T b) { return b < a ? b : a; }
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T max(T a, T b) { return a < b ? b : a; }
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T atan2(T y, T x) {
  if consteval {
    return static_cast<T>(cm::atan2(static_cast<double>(y), static_cast<double>(x)));
  } else {
    return std::atan2(y, x);
  }
}
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T pow(T x, T y) {
  if consteval {
    return static_cast<T>(cm::pow(static_cast<double>(x), static_cast<double>(y)));
  } else {
    return std::pow(x, y);
  }
}
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T where(bool cond, T a, T b) { return cond ? a : b; }
// Comparison results as 1/0 and selection on a nonzero condition, as the generated code uses them
// (overloaded lane-wise for Batch).
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T lt(T a, T b) { return a < b ? T(1) : T(0); }
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T le(T a, T b) { return a <= b ? T(1) : T(0); }
template <std::floating_point T>
CSYM_ALWAYS_INLINE constexpr T select(T cond, T a, T b) { return cond != T(0) ? a : b; }

}  // namespace csym
