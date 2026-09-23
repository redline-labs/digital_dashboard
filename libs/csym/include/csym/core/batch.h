#pragma once

// Batch<T, W>: W independent values of T (one per factor instance), used as the scalar type of a
// Function to evaluate W instances of the same generated code at once:
//
//   using B4 = csym::Batch<double, 4>;
//   using Fn4 = csym::Function<f, Pose2<B4>, Vector3<B4>, B4>;
//   auto pose = csym::gather(std::array<Pose2<double>, 4>{...});   // lane i = element i
//   auto res = Fn4::eval(pose, vel, dt);                            // 4 residuals, one per lane
//   std::array<Vector3<double>, 4> r = csym::scatter(res);
//
// Lanes are stored in a native SIMD vector (the GNU `vector_size` extension, which GCC and clang both
// implement; other compilers fall back to plain arrays), so arithmetic compiles to SIMD instructions
// directly. Transcendentals use branch-free kernels (standard argument reduction and fdlibm's minimax
// polynomials) written over that vector type, since calling libm per lane would serialize exactly the work
// batching is meant to parallelize, and the auto-vectorizer does not reliably vectorize them. Kernels are
// accurate to a few ulp over normal ranges; see tests/batch_test.cpp for measured bounds. Known
// differences from <cmath>: pow(x, y) of negative x is NaN (even for integer y), and atan2(±inf, ±inf) is
// NaN. float lanes are computed in double.
//
// The kernels are not -ffast-math safe in general (they rely on IEEE special values), but they avoid the
// (x + 2^52) - 2^52 rounding trick that fast-math reassociation would break.

#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "csym/config.h"
#include "csym/core/batch_fwd.h"
#include "csym/core/storage.h"
#include "csym/math/cmath.h"

namespace csym {

#if (defined(__GNUC__) || defined(__clang__)) && !defined(CSYM_NO_VECTOR_EXT)
#define CSYM_HAS_VECTOR_EXT 1
#else
#define CSYM_HAS_VECTOR_EXT 0
#endif

namespace vm {

// Math kernels written once over a value type V, which is either double (constant evaluation, and
// compilers without GNU vector extensions) or a native vector of doubles (GCC and clang implement the
// same `vector_size` extension). In the vector case every operation below is a SIMD instruction; nothing
// depends on the auto-vectorizer. Comparisons yield bool or an integer lane mask, and `c ? a : b` is a
// per-lane select for vectors, so one source serves both.

// I: integer lane type matching V. The primary template covers native vectors of doubles.
template <class V>
struct Traits {
#if CSYM_HAS_VECTOR_EXT
  typedef std::int64_t I __attribute__((vector_size(sizeof(V))));
  static constexpr V splat(double c) { return V{} + c; }
#endif
};
template <>
struct Traits<double> {
  using I = std::int64_t;
  static constexpr double splat(double c) { return c; }
};

template <class V>
CSYM_ALWAYS_INLINE constexpr V splat(double c) {
  return Traits<V>::splat(c);
}
template <class V>
CSYM_ALWAYS_INLINE constexpr auto bits(V x) {
  return std::bit_cast<typename Traits<V>::I>(x);
}
template <class V, class I>
CSYM_ALWAYS_INLINE constexpr V from_bits(I b) {
  return std::bit_cast<V>(b);
}
template <class V>
CSYM_ALWAYS_INLINE constexpr auto to_int(V x) {
  if constexpr (std::is_same_v<V, double>) return static_cast<std::int64_t>(x);
  else return __builtin_convertvector(x, typename Traits<V>::I);
}
template <class V, class I>
CSYM_ALWAYS_INLINE constexpr V to_double(I x) {
  if constexpr (std::is_same_v<V, double>) return static_cast<double>(x);
  else return __builtin_convertvector(x, V);
}
template <class V>
CSYM_ALWAYS_INLINE constexpr V rint(V x) {
  if consteval {
    if constexpr (std::is_same_v<V, double>) return cm::round(x);  // ties differ, irrelevant here
    else {
      for (std::size_t i = 0; i < sizeof(V) / sizeof(double); ++i) x[i] = cm::round(x[i]);
      return x;
    }
  } else {
    if constexpr (std::is_same_v<V, double>) return std::rint(x);
    else {
      for (std::size_t i = 0; i < sizeof(V) / sizeof(double); ++i) x[i] = std::rint(x[i]);  // frintx / roundpd
      return x;
    }
  }
}
template <class V>
CSYM_ALWAYS_INLINE constexpr V sqrt(V x) {
  if consteval {
    if constexpr (std::is_same_v<V, double>) return cm::sqrt(x);
    else {
      for (std::size_t i = 0; i < sizeof(V) / sizeof(double); ++i) x[i] = cm::sqrt(x[i]);
      return x;
    }
  } else {
    if constexpr (std::is_same_v<V, double>) return std::sqrt(x);
    else {
      for (std::size_t i = 0; i < sizeof(V) / sizeof(double); ++i) x[i] = std::sqrt(x[i]);
      return x;
    }
  }
}
template <class V>
CSYM_ALWAYS_INLINE constexpr V fabs(V x) {
  return from_bits<V>(bits(x) & 0x7fffffffffffffffll);
}
template <class V>
CSYM_ALWAYS_INLINE constexpr V copysign(V mag, V sgn) {
  return from_bits<V>((bits(mag) & 0x7fffffffffffffffll) | (bits(sgn) & (-0x7fffffffffffffffll - 1)));
}
template <class V>
CSYM_ALWAYS_INLINE constexpr auto signbit(V x) {
  return bits(x) < 0;
}
// 2^k for k in [-1022, 1023].
template <class V, class I>
CSYM_ALWAYS_INLINE constexpr V pow2(I k) {
  return from_bits<V>((k + 1023) << 52);
}

template <class V>
struct SinCos {
  V s, c;
};
// Cody-Waite reduction by pi/2 (3-part constant, exact products for |k| < 2^20) and fdlibm's kernel
// polynomials on [-pi/4, pi/4].
template <class V>
CSYM_ALWAYS_INLINE constexpr SinCos<V> sincos(V x) {
  const V k = rint(x * 6.36619772367581382433e-01);
  V r = x - k * 1.57079632673412561417e+00;
  r = r - k * 6.07710050630396597660e-11;
  r = r - k * 2.02226624871116645580e-21;
  const V z = r * r;
  const V s =
      r + r * z *
              (-1.66666666666666324348e-01 +
               z * (8.33333333332248946124e-03 +
                    z * (-1.98412698298579493134e-04 +
                         z * (2.75573137070700676789e-06 + z * (-2.50507602534068634195e-08 + z * 1.58969099521155010221e-10)))));
  const V c =
      1.0 - 0.5 * z +
      z * z *
          (4.16666666666666019037e-02 +
           z * (-1.38888888888741095749e-03 +
                z * (2.48015872894767294178e-05 +
                     z * (-2.75573143513906633035e-07 + z * (2.08757232129817482790e-09 + z * -1.13596475577881948265e-11)))));
  const auto q = to_int(k == k ? k : splat<V>(0.0)) & 3;
  const V sn = q == 0 ? s : q == 1 ? c : q == 2 ? -s : -c;
  const V cs = q == 0 ? c : q == 1 ? -s : q == 2 ? -c : s;
  return {sn, cs};
}

// fdlibm atan: reduction to |x| < 7/16 via atan(x) = atan(c) + atan((x - c) / (1 + x c)), c in
// {0.5, 1, 1.5, inf}, then an odd minimax polynomial. Interval choice is branch-free.
template <class V>
CSYM_ALWAYS_INLINE constexpr V atan(V x0) {
  const V ax = fabs(x0);
  const auto i0 = ax < 0.4375, i1 = ax < 0.6875, i2 = ax < 1.1875, i3 = ax < 2.4375;
  const V one = splat<V>(1.0);
  const V num = i0 ? ax : i1 ? 2.0 * ax - 1.0 : i2 ? ax - 1.0 : i3 ? ax - 1.5 : -one;
  const V den = i0 ? one : i1 ? 2.0 + ax : i2 ? ax + 1.0 : i3 ? 1.0 + 1.5 * ax : ax;
  const V hi = i0 ? splat<V>(0.0)
             : i1 ? splat<V>(4.63647609000806093515e-01)
             : i2 ? splat<V>(7.85398163397448278999e-01)
             : i3 ? splat<V>(9.82793723247329054082e-01)
                  : splat<V>(1.57079632679489655800e+00);
  const V lo = i0 ? splat<V>(0.0)
             : i1 ? splat<V>(2.26987774529616870924e-17)
             : i2 ? splat<V>(3.06161699786838301793e-17)
             : i3 ? splat<V>(1.39033110312309984516e-17)
                  : splat<V>(6.12323399573676603587e-17);
  const V x = num / den;
  const V z = x * x, w = z * z;
  const V s1 =
      z * (3.33333333333329318027e-01 +
           w * (1.42857142725034663711e-01 +
                w * (9.09088713343650656196e-02 +
                     w * (6.66107313738753120669e-02 + w * (4.97687799461593236017e-02 + w * 1.62858201153657823623e-02)))));
  const V s2 =
      w * (-1.99999999998764832476e-01 +
           w * (-1.11111104054623557880e-01 +
                w * (-7.69187620504482999495e-02 + w * (-5.83357013379057348645e-02 + w * -3.65315727442169155270e-02))));
  const V r = i0 ? x - x * (s1 + s2) : hi - ((x * (s1 + s2) - lo) - x);
  return copysign(r, x0);
}

template <class V>
CSYM_ALWAYS_INLINE constexpr V atan2(V y, V x) {
  const V ay = fabs(y), ax = fabs(x);
  V a = atan(ay / ax);                // x == 0, y != 0: atan(inf) = pi/2
  a = ay == 0.0 ? splat<V>(0.0) : a;  // also avoids 0/0
  a = signbit(x) ? 3.14159265358979311600e+00 - a : a;
  return copysign(a, y);
}

// Reduction x = k ln2 + r and e^r - 1 (fdlibm exp).
template <class V>
struct ExpReduced {
  V em;
  decltype(to_int(V{})) k;
};
template <class V>
CSYM_ALWAYS_INLINE constexpr ExpReduced<V> exp_reduce(V x) {
  const V k = rint(x * 1.44269504088896338700e+00);
  const V hi = x - k * 6.93147180369123816490e-01;
  const V lo = k * 1.90821492927058770002e-10;
  const V r = hi - lo;
  const V t = r * r;
  const V c =
      r - t * (1.66666666666666019037e-01 +
               t * (-2.77777777770155933842e-03 +
                    t * (6.61375632143793436117e-05 + t * (-1.65339022054652515390e-06 + t * 4.13813679705723846039e-08))));
  const V em = -((lo - (r * c) / (2.0 - c)) - hi);
  return {em, to_int(k)};
}

template <class V>
CSYM_ALWAYS_INLINE constexpr V exp(V x) {
  const auto nan = x != x;
  const V xc = nan ? splat<V>(0.0) : (x > 709.8 ? splat<V>(709.8) : (x < -745.2 ? splat<V>(-745.2) : x));
  const auto red = exp_reduce(xc);
  // 2^k in two factors so that subnormal and near-overflow results scale correctly.
  const auto k1 = red.k >> 1, k2 = red.k - k1;
  V y = (1.0 + red.em) * pow2<V>(k1) * pow2<V>(k2);
  y = x > 709.782712893384 ? splat<V>(cm::inf) : y;
  y = x < -745.1332191019412 ? splat<V>(0.0) : y;
  return nan ? x : y;
}

template <class V>
CSYM_ALWAYS_INLINE constexpr V expm1(V x) {
  const auto nan = x != x;
  const V xc = nan ? splat<V>(0.0) : (x > 709.0 ? splat<V>(709.0) : (x < -40.0 ? splat<V>(-40.0) : x));
  const auto red = exp_reduce(xc);
  const V s = pow2<V>(red.k);
  const V y = red.em * s + (s - 1.0);  // exact for k == 0, so accurate near zero
  return nan ? x : (x > 709.0 ? splat<V>(cm::inf) : (x < -40.0 ? splat<V>(-1.0) : y));
}

// fdlibm log: x = 2^e * m, m in [sqrt(1/2), sqrt(2)), log(m) via s = f / (2 + f).
template <class V>
CSYM_ALWAYS_INLINE constexpr V log(V x) {
  const auto sub = x < 0x1p-1022;
  const V xs = sub ? x * 0x1p54 : x;
  const auto b = bits(xs);
  using I = decltype(b);
  auto e = ((b >> 52) & 0x7ff) - 1023 - (sub ? I{} + 54 : I{});
  V m = from_bits<V>((b & 0x000fffffffffffffll) | 0x3ff0000000000000ll);  // [1, 2)
  const auto big = m > 1.41421356237309504880;
  m = big ? m * 0.5 : m;
  e = big ? e + 1 : e;
  const V f = m - 1.0;
  const V s = f / (2.0 + f);
  const V z = s * s, w = z * z;
  const V t1 = w * (3.999999999940941908e-01 + w * (2.222219843214978396e-01 + w * 1.531383769920937332e-01));
  const V t2 = z * (6.666666666666735130e-01 +
                    w * (2.857142874366239149e-01 + w * (1.818357216161805012e-01 + w * 1.479819860511658591e-01)));
  const V hfsq = 0.5 * f * f;
  const V de = to_double<V>(e);
  V r = de * 6.93147180369123816490e-01 - ((hfsq - (s * (hfsq + t1 + t2) + de * 1.90821492927058770002e-10)) - f);
  r = x == 0.0 ? splat<V>(-cm::inf) : r;
  r = x == cm::inf ? x : r;
  return (x < 0.0 || x != x) ? splat<V>(cm::nan) : r;
}

template <class V>
CSYM_ALWAYS_INLINE constexpr V tanh(V x) {
  const V a = fabs(x);
  const V em = expm1(2.0 * (a > 22.0 ? splat<V>(22.0) : a));
  const V t = a > 22.0 ? splat<V>(1.0) : em / (em + 2.0);
  return x != x ? x : copysign(t, x);
}

template <class V>
CSYM_ALWAYS_INLINE constexpr V asin(V x) {
  return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}
template <class V>
CSYM_ALWAYS_INLINE constexpr V acos(V x) {
  return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}
template <class V>
CSYM_ALWAYS_INLINE constexpr V pow(V x, V y) {
  return y == 0.0 ? splat<V>(1.0) : exp(y * log(x));
}

}  // namespace vm

namespace detail {
constexpr bool is_pow2(std::size_t w) { return w != 0 && (w & (w - 1)) == 0; }
// Lane storage: a native vector when available (and W is a power of two), otherwise an array.
#if CSYM_HAS_VECTOR_EXT
template <class T, std::size_t W, bool Native = is_pow2(W)>
struct lane_storage {
  using type = T[W];
};
template <class T, std::size_t W>
struct lane_storage<T, W, true> {
  typedef T type __attribute__((vector_size(sizeof(T) * W)));
};
template <std::size_t W>
using dvec = typename lane_storage<double, W>::type;
#else
template <class T, std::size_t W, bool Native = false>
struct lane_storage {
  using type = T[W];
};
#endif
}  // namespace detail

template <class T, std::size_t W>
struct Batch {
  static_assert(std::is_floating_point_v<T>, "Batch lanes must be float or double");
  using value_type = T;
  static constexpr std::size_t width = W;
  static constexpr bool native = CSYM_HAS_VECTOR_EXT && detail::is_pow2(W) && W > 1;
  using storage_type = typename detail::lane_storage<T, W>::type;

  storage_type v;

  constexpr Batch() = default;
  template <class A>
    requires std::is_arithmetic_v<A>
  constexpr Batch(A x) : Batch(from_fn([x](std::size_t) { return static_cast<T>(x); })) {}  // NOLINT: broadcast

  // Batch whose lane i is f(i). Native vectors are built with a brace initializer: GCC's constant
  // evaluator does not allow assigning individual vector elements.
  template <class F>
  CSYM_ALWAYS_INLINE static constexpr Batch from_fn(F f) {
    Batch r;
    if constexpr (native) {
      r.v = [&]<std::size_t... I>(std::index_sequence<I...>) {
        return storage_type{static_cast<T>(f(I))...};
      }(std::make_index_sequence<W>{});
    } else {
      for (std::size_t i = 0; i < W; ++i) r.v[i] = static_cast<T>(f(i));
    }
    return r;
  }

  constexpr T operator[](std::size_t i) const { return v[i]; }
  constexpr void set(std::size_t i, T x) {
    *this = from_fn([&](std::size_t j) { return j == i ? x : T(v[j]); });
  }
  // Lane access as a reference only works for array storage; use set() in generic code.
  constexpr T& operator[](std::size_t i) requires(!native) { return v[i]; }

#define CSYM_BATCH_BINOP(op)                                                                 \
  CSYM_ALWAYS_INLINE friend constexpr Batch operator op(const Batch& a, const Batch& b) {    \
    Batch r;                                                                                 \
    if constexpr (native) {                                                                  \
      r.v = a.v op b.v;                                                                      \
    } else {                                                                                 \
      for (std::size_t i = 0; i < W; ++i) r.v[i] = a.v[i] op b.v[i];                         \
    }                                                                                        \
    return r;                                                                                \
  }                                                                                          \
  CSYM_ALWAYS_INLINE constexpr Batch& operator op##=(const Batch& b) { return *this = *this op b; }
  CSYM_BATCH_BINOP(+)
  CSYM_BATCH_BINOP(-)
  CSYM_BATCH_BINOP(*)
  CSYM_BATCH_BINOP(/)
#undef CSYM_BATCH_BINOP
  CSYM_ALWAYS_INLINE friend constexpr Batch operator-(const Batch& a) {
    Batch r;
    if constexpr (native) r.v = -a.v;
    else
      for (std::size_t i = 0; i < W; ++i) r.v[i] = -a.v[i];
    return r;
  }
  CSYM_ALWAYS_INLINE friend constexpr Batch operator+(const Batch& a) { return a; }
};

namespace detail {
// Applies a kernel to every lane: on a native vector of doubles at runtime, lane by lane (scalar
// kernel) in constant evaluation or without vector support.
template <class T, std::size_t W, class F>
CSYM_ALWAYS_INLINE constexpr Batch<T, W> lanes(const Batch<T, W>& a, F f) {
  Batch<T, W> r;
#if CSYM_HAS_VECTOR_EXT
  if constexpr (Batch<T, W>::native) {
    if !consteval {
      if constexpr (std::is_same_v<T, double>) r.v = f(a.v);
      else r.v = __builtin_convertvector(f(__builtin_convertvector(a.v, dvec<W>)), typename Batch<T, W>::storage_type);
      return r;
    }
  }
#endif
  return Batch<T, W>::from_fn([&](std::size_t i) { return f(static_cast<double>(a.v[i])); });
}
template <class T, std::size_t W, class F>
CSYM_ALWAYS_INLINE constexpr Batch<T, W> lanes(const Batch<T, W>& a, const Batch<T, W>& b, F f) {
  Batch<T, W> r;
#if CSYM_HAS_VECTOR_EXT
  if constexpr (Batch<T, W>::native) {
    if !consteval {
      if constexpr (std::is_same_v<T, double>) r.v = f(a.v, b.v);
      else
        r.v = __builtin_convertvector(f(__builtin_convertvector(a.v, dvec<W>), __builtin_convertvector(b.v, dvec<W>)),
                                      typename Batch<T, W>::storage_type);
      return r;
    }
  }
#endif
  return Batch<T, W>::from_fn([&](std::size_t i) { return f(static_cast<double>(a.v[i]), static_cast<double>(b.v[i])); });
}
}  // namespace detail

// ---- math on batches (lane-wise) --------------------------------------------------------------------
#define CSYM_BATCH_UNARY(name, expr)                                                   \
  template <class T, std::size_t W>                                                    \
  CSYM_ALWAYS_INLINE constexpr Batch<T, W> name(const Batch<T, W>& a) {                \
    return detail::lanes(a, [](auto x) CSYM_LAMBDA_INLINE { return expr; });                              \
  }
CSYM_BATCH_UNARY(sin, vm::sincos(x).s)
CSYM_BATCH_UNARY(cos, vm::sincos(x).c)
CSYM_BATCH_UNARY(tan, vm::sincos(x).s / vm::sincos(x).c)
CSYM_BATCH_UNARY(atan, vm::atan(x))
CSYM_BATCH_UNARY(sqrt, vm::sqrt(x))
CSYM_BATCH_UNARY(asin, vm::asin(x))
CSYM_BATCH_UNARY(acos, vm::acos(x))
CSYM_BATCH_UNARY(exp, vm::exp(x))
CSYM_BATCH_UNARY(log, vm::log(x))
CSYM_BATCH_UNARY(tanh, vm::tanh(x))
CSYM_BATCH_UNARY(abs, vm::fabs(x))
CSYM_BATCH_UNARY(sign, x > 0.0 ? decltype(x)(vm::splat<decltype(x)>(1.0)) : (x < 0.0 ? vm::splat<decltype(x)>(-1.0) : vm::splat<decltype(x)>(0.0)))
CSYM_BATCH_UNARY(sign_no_zero, x < 0.0 ? vm::splat<decltype(x)>(-1.0) : vm::splat<decltype(x)>(1.0))
#undef CSYM_BATCH_UNARY

#define CSYM_BATCH_BINARY(name, expr)                                                         \
  template <class T, std::size_t W>                                                           \
  CSYM_ALWAYS_INLINE constexpr Batch<T, W> name(const Batch<T, W>& a, const Batch<T, W>& b) { \
    return detail::lanes(a, b, [](auto x, auto y) CSYM_LAMBDA_INLINE { return expr; });                          \
  }
CSYM_BATCH_BINARY(atan2, vm::atan2(x, y))
CSYM_BATCH_BINARY(pow, vm::pow(x, y))
CSYM_BATCH_BINARY(min, y < x ? y : x)
CSYM_BATCH_BINARY(max, x < y ? y : x)
// Comparisons as 1/0 and selection, matching the scalar forms in cmath.h.
CSYM_BATCH_BINARY(lt, x < y ? vm::splat<decltype(x)>(1.0) : vm::splat<decltype(x)>(0.0))
CSYM_BATCH_BINARY(le, x <= y ? vm::splat<decltype(x)>(1.0) : vm::splat<decltype(x)>(0.0))
CSYM_BATCH_BINARY(eq, x == y ? vm::splat<decltype(x)>(1.0) : vm::splat<decltype(x)>(0.0))
#undef CSYM_BATCH_BINARY

template <class T, std::size_t W>
CSYM_ALWAYS_INLINE constexpr Batch<T, W> floor(const Batch<T, W>& a) {
  return Batch<T, W>::from_fn([&](std::size_t i) -> T {
    if consteval {
      return static_cast<T>(cm::floor(a.v[i]));
    } else {
      return std::floor(a.v[i]);
    }
  });
}
template <class T, std::size_t W>
CSYM_ALWAYS_INLINE constexpr Batch<T, W> copysign_no_zero(const Batch<T, W>& a, const Batch<T, W>& b) {
  return abs(a) * sign_no_zero(b);
}

// cond != 0 ? a : b per lane.
template <class T, std::size_t W>
CSYM_ALWAYS_INLINE constexpr Batch<T, W> select(const Batch<T, W>& cond, const Batch<T, W>& a, const Batch<T, W>& b) {
  Batch<T, W> r;
  if constexpr (Batch<T, W>::native) {
    if !consteval {
      r.v = cond.v != T(0) ? a.v : b.v;
      return r;
    }
  }
  return Batch<T, W>::from_fn([&](std::size_t i) { return cond.v[i] != T(0) ? a.v[i] : b.v[i]; });
}
template <class T, std::size_t W>
CSYM_ALWAYS_INLINE constexpr Batch<T, W> where(const Batch<T, W>& cond, const Batch<T, W>& a, const Batch<T, W>& b) {
  return select(cond, a, b);
}

// ---- gather / scatter ----------------------------------------------------------------------------------
// Packs W values of any storable type (Pose2<double>, Vector3<double>, double, ...) into one value over
// Batch lanes, and back.
template <class T, std::size_t W>
constexpr auto gather(const std::array<T, W>& xs) {
  using S = scalar_of_t<T>;
  using B = Batch<S, W>;
  using TB = rebind_t<T, B>;
  constexpr std::size_t n = storage_dim<T>;
  std::array<std::array<S, n>, W> lanes_in{};
  for (std::size_t w = 0; w < W; ++w) storage<T>::to(xs[w], lanes_in[w].data());
  std::array<B, n> flat;
  for (std::size_t k = 0; k < n; ++k) flat[k] = B::from_fn([&](std::size_t w) { return lanes_in[w][k]; });
  return storage<TB>::from(flat.data());
}

template <class TB>
constexpr auto scatter(const TB& x) {
  using B = scalar_of_t<TB>;
  using S = typename B::value_type;
  constexpr std::size_t W = B::width;
  using T = rebind_t<TB, S>;
  constexpr std::size_t n = storage_dim<TB>;
  std::array<B, n> flat;
  storage<TB>::to(x, flat.data());
  std::array<T, W> out;
  for (std::size_t w = 0; w < W; ++w) {
    std::array<S, n> s;
    for (std::size_t k = 0; k < n; ++k) s[k] = flat[k].v[w];
    out[w] = storage<T>::from(s.data());
  }
  return out;
}

}  // namespace csym
