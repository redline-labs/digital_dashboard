// Batch<T, W>: vector math kernel accuracy vs <cmath>, and batched Function evaluation vs scalar.

#include <cmath>
#include <cstdio>
#include <limits>
#include <random>

#include "vehicle_models.h"
#include "check.h"

using namespace csym;
using B4 = Batch<double, 4>;

namespace {

double ulps(double got, double ref) {
  if (std::isnan(ref)) return std::isnan(got) ? 0 : 1e30;
  if (got == ref) return 0;
  if (std::isinf(ref) || std::isinf(got)) return 1e30;
  const double spacing = std::nextafter(std::fabs(ref), INFINITY) - std::fabs(ref);
  return std::fabs(got - ref) / spacing;
}

// Max ulp error of a batch kernel against the <cmath> reference over random inputs.
template <class F, class G>
double max_ulp(F batch_fn, G ref_fn, double lo, double hi, std::size_t n = 200000) {
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> U(lo, hi);
  double worst = 0;
  for (std::size_t i = 0; i < n; i += 4) {
    B4 x;
    for (std::size_t k = 0; k < 4; ++k) x.set(k, U(rng));
    const B4 y = batch_fn(x);
    for (std::size_t k = 0; k < 4; ++k) worst = std::max(worst, ulps(y[k], ref_fn(x[k])));
  }
  return worst;
}

void check_kernel(const char* name, double worst, double bound) {
  std::printf("  %-6s max %.1f ulp (bound %.0f)\n", name, worst, bound);
  CHECK(worst <= bound);
}

}  // namespace

// Batches work in constant evaluation as well.
static_assert([] {
  const B4 x{0.5};
  return csym::sin(x)[2] > 0.479 && csym::sin(x)[2] < 0.4795;
}());

int main() {
  std::printf("kernel accuracy:\n");
  check_kernel("sin", max_ulp([](B4 x) { return sin(x); }, [](double x) { return std::sin(x); }, -100, 100), 2);
  check_kernel("cos", max_ulp([](B4 x) { return cos(x); }, [](double x) { return std::cos(x); }, -100, 100), 2);
  check_kernel("tan", max_ulp([](B4 x) { return tan(x); }, [](double x) { return std::tan(x); }, -1.5, 1.5), 4);
  check_kernel("atan", max_ulp([](B4 x) { return atan(x); }, [](double x) { return std::atan(x); }, -50, 50), 2);
  check_kernel("exp", max_ulp([](B4 x) { return exp(x); }, [](double x) { return std::exp(x); }, -700, 700), 2);
  check_kernel("log", max_ulp([](B4 x) { return log(x); }, [](double x) { return std::log(x); }, 1e-300, 1e300), 2);
  check_kernel("log1", max_ulp([](B4 x) { return log(x); }, [](double x) { return std::log(x); }, 0.5, 2.0), 2);
  check_kernel("tanh", max_ulp([](B4 x) { return tanh(x); }, [](double x) { return std::tanh(x); }, -5, 5), 3);
  check_kernel("tanh0", max_ulp([](B4 x) { return tanh(x); }, [](double x) { return std::tanh(x); }, -1e-6, 1e-6), 3);
  check_kernel("asin", max_ulp([](B4 x) { return asin(x); }, [](double x) { return std::asin(x); }, -0.99, 0.99), 4);
  check_kernel("acos", max_ulp([](B4 x) { return acos(x); }, [](double x) { return std::acos(x); }, -0.99, 0.99), 4);
  {
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> U(-10, 10);
    double worst = 0;
    for (std::size_t i = 0; i < 100000; ++i) {
      // Initialized first: set() rebuilds the whole batch from its other lanes, so filling a
      // default-constructed (indeterminate, like a double) batch one lane at a time reads the lanes
      // not yet written.
      B4 y{0.0}, x{0.0};
      for (std::size_t k = 0; k < 4; ++k) y.set(k, U(rng)), x.set(k, U(rng));
      const B4 r = atan2(y, x);
      for (std::size_t k = 0; k < 4; ++k) worst = std::max(worst, ulps(r[k], std::atan2(y[k], x[k])));
    }
    check_kernel("atan2", worst, 2);
  }
  // Special values.
  {
    const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
    const B4 e = exp(B4{0.0});
    CHECK(e[0] == 1.0);
    B4 x;
    x.set(0, 1000); x.set(1, -1000); x.set(2, nan); x.set(3, -740);  // overflow, underflow, NaN, subnormal result
    const B4 ex = exp(x);
    CHECK(ex[0] == inf && ex[1] == 0.0 && std::isnan(ex[2]) && ulps(ex[3], std::exp(-740.0)) < 2);
    x.set(0, 0); x.set(1, -1); x.set(2, inf); x.set(3, 4.9e-320);  // log of zero, negative, inf, subnormal
    const B4 lg = log(x);
    CHECK(lg[0] == -inf && std::isnan(lg[1]) && lg[2] == inf && ulps(lg[3], std::log(4.9e-320)) < 2);
    B4 ys, xs;  // atan2 on the axes and signed zeros
    ys.set(0, 0.0); xs.set(0, -1.0);
    ys.set(1, -0.0); xs.set(1, -1.0);
    ys.set(2, 1.0); xs.set(2, 0.0);
    ys.set(3, 0.0); xs.set(3, 0.0);
    const B4 a = atan2(ys, xs);
    for (std::size_t k = 0; k < 4; ++k) CHECK(a[k] == std::atan2(ys[k], xs[k]));
  }

  // Batched Function vs scalar, lane by lane: bicycle factor linearization and per-argument blocks.
  using P2 = Pose2<double>;
  using V2 = Vector2<double>;
  using V3 = Vector3<double>;
  using V4 = Vector<double, 4>;
  using V6 = Vector<double, 6>;
  using Scalar1 = Function<vehicle::bicycle_factor, P2, V3, P2, V3, V2, V4, V4, V4, double, V6, double>;
  using Batched = Function<vehicle::bicycle_factor, Pose2<B4>, Vector3<B4>, Pose2<B4>, Vector3<B4>, Vector2<B4>,
                           Vector<B4, 4>, Vector<B4, 4>, Vector<B4, 4>, B4, Vector<B4, 6>, B4>;
  std::array<P2, 4> pk, pk1;
  std::array<V3, 4> vk, vk1;
  std::array<V2, 4> u;
  std::array<double, 4> dt;
  for (std::size_t i = 0; i < 4; ++i) {
    const double x = static_cast<double>(i);  // lane number, as a value
    pk[i] = P2::from_angle_position(0.1 * x, 10.0 + x, 5.0 - x);
    pk1[i] = P2::from_angle_position(0.1 * x + 0.01, 10.2 + x, 5.05 - x);
    vk[i] = V3{20.0 + 3 * x, 0.5 - 0.2 * x, 0.1 * x - 0.1};
    vk1[i] = V3{20.1 + 3 * x, 0.45 - 0.2 * x, 0.1 * x - 0.08};
    u[i] = V2{0.02 * (x - 1.5), 500.0 * x};
    dt[i] = 0.01 + 0.002 * x;
  }
  const V4 chassis{750.0, 1100.0, 1.3, 1.5}, tf{10.0, 1.4, 7000.0, 0.1}, tr{11.0, 1.4, 7500.0, 0.1};
  const V6 w{10, 10, 20, 5, 5, 8};
  const auto lin = Batched::linearize<0, 1, 2, 3>(
      gather(pk), gather(vk), gather(pk1), gather(vk1), gather(u), gather(std::array<V4, 4>{chassis, chassis, chassis, chassis}),
      gather(std::array<V4, 4>{tf, tf, tf, tf}), gather(std::array<V4, 4>{tr, tr, tr, tr}), gather(dt),
      gather(std::array<V6, 4>{w, w, w, w}), B4{1e-9});
  const auto res = scatter(lin.residual);
  const auto jac = scatter(lin.jacobian);
  const auto hess = scatter(lin.hessian);
  Batched::Block<1> Hv;
  const auto value = Batched::evaluate<0, 1, 2, 3>(gather(pk), gather(vk), gather(pk1), gather(vk1), gather(u),
                                                   gather(std::array<V4, 4>{chassis, chassis, chassis, chassis}),
                                                   gather(std::array<V4, 4>{tf, tf, tf, tf}),
                                                   gather(std::array<V4, 4>{tr, tr, tr, tr}), gather(dt),
                                                   gather(std::array<V6, 4>{w, w, w, w}), B4{1e-9}, nullptr, &Hv,
                                                   nullptr, nullptr);
  const auto hv = scatter(Hv);
  const auto vals = scatter(value);
  for (std::size_t i = 0; i < 4; ++i) {
    const auto ref = Scalar1::linearize<0, 1, 2, 3>(pk[i], vk[i], pk1[i], vk1[i], u[i], chassis, tf, tr, dt[i], w, 1e-9);
    for (std::size_t k = 0; k < 6; ++k) {
      CHECK_CLOSE(res[i][k], ref.residual[k], 1e-12, 1e-12);
      CHECK_CLOSE(vals[i][k], ref.residual[k], 1e-12, 1e-12);
    }
    for (std::size_t k = 0; k < ref.jacobian.data.size(); ++k) CHECK_CLOSE(jac[i].data[k], ref.jacobian.data[k], 1e-11, 1e-11);
    for (std::size_t k = 0; k < ref.hessian.data.size(); ++k) CHECK_CLOSE(hess[i].data[k], ref.hessian.data[k], 1e-10, 1e-10);
    for (std::size_t r = 0; r < 6; ++r)
      for (std::size_t c = 0; c < 3; ++c) CHECK_CLOSE(hv[i](r, c), ref.jacobian(r, 3 + c), 1e-11, 1e-11);
  }

  // float lanes
  using F8 = Batch<float, 8>;
  using TireF8 = Function<vehicle::tire_fy, F8, F8, F8, F8, F8, F8, F8, F8, F8, F8, F8>;
  std::array<float, 8> vx;
  for (std::size_t i = 0; i < 8; ++i) vx[i] = 5.0f + 4.0f * static_cast<float>(i);
  const auto fy = scatter(TireF8::eval(gather(vx), F8{0.8f}, F8{0.3f}, F8{0.05f}, F8{1.2f}, F8{10.f}, F8{1.4f},
                                       F8{1.05f}, F8{4200.f}, F8{0.2f}, F8{1e-6f}));
  for (std::size_t i = 0; i < 8; ++i)
    CHECK_CLOSE(fy[i], vehicle::tire_fy(double(vx[i]), 0.8, 0.3, 0.05, 1.2, 10.0, 1.4, 1.05, 4200.0, 0.2, 1e-6), 1e-5,
                1e-3);
  return check::report("batch");
}
