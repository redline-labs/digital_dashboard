// Throughput of the bicycle factor: scalar calls vs Batch<T, W> calls (ns per factor instance).
#include <chrono>
#include <cstdio>

#include "../tests/vehicle_models.h"

using namespace csym;

volatile double sink;

template <class F>
double best_ns(F&& f, int iters) {
  double best = 1e30;
  for (int rep = 0; rep < 5; ++rep) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) f(i);
    best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / iters);
  }
  return best;
}

template <class M>
double total(const M& m) {
  double s = 0;
  for (const auto& x : m.data) {
    if constexpr (is_batch_v<std::remove_cvref_t<decltype(x)>>)
      for (std::size_t l = 0; l < x.width; ++l) s += x[l];
    else
      s += x;
  }
  return s;
}

template <class S>
struct Bench {
  using P2 = Pose2<S>;
  using V2 = Vector2<S>;
  using V3 = Vector3<S>;
  using V4 = Vector<S, 4>;
  using V6 = Vector<S, 6>;
  using Fn = Function<vehicle::bicycle_factor, P2, V3, P2, V3, V2, V4, V4, V4, S, V6, S>;
  static constexpr std::size_t lanes = [] {
    if constexpr (is_batch_v<S>) return S::width;
    else return std::size_t{1};
  }();

  static void run(const char* name) {
    const P2 pk = P2::from_angle_position(S(0.1), S(10.0), S(5.0)), pk1 = P2::from_angle_position(S(0.102), S(10.25), S(5.03));
    const V3 vk1{S(25.1), S(0.49), S(0.21)};
    const V2 u{S(0.04), S(800.0)};
    const V4 ch{S(750.0), S(1100.0), S(1.3), S(1.5)}, tf{S(10.0), S(1.4), S(7000.0), S(0.1)}, tr{S(11.0), S(1.4), S(7500.0), S(0.1)};
    const V6 w{S(10), S(10), S(20), S(5), S(5), S(8)};
    auto vel = [](int i) { return V3{S(25.0 + i * 1e-9), S(0.5), S(0.2)}; };
    const int iters = 100000;
    const double t_eval = best_ns([&](int i) { sink = total(Fn::eval(pk, vel(i), pk1, vk1, u, ch, tf, tr, S(0.01), w, S(1e-9))); }, iters);
    typename Fn::template Block<0> H0;
    typename Fn::template Block<1> H1;
    typename Fn::template Block<2> H2;
    typename Fn::template Block<3> H3;
    const double t_blocks = best_ns([&](int i) {
      const auto r = Fn::template evaluate<0, 1, 2, 3>(pk, vel(i), pk1, vk1, u, ch, tf, tr, S(0.01), w, S(1e-9), &H0, &H1, &H2, &H3);
      sink = total(r) + total(H0) + total(H1) + total(H2) + total(H3);
    }, iters);
    const double t_lin = best_ns([&](int i) {
      const auto r = Fn::template linearize<0, 1, 2, 3>(pk, vel(i), pk1, vk1, u, ch, tf, tr, S(0.01), w, S(1e-9));
      sink = total(r.hessian) + total(r.rhs) + total(r.jacobian) + total(r.residual);
    }, iters);
    std::printf("%-16s per factor:  residual %6.1f ns   residual + 4 blocks %6.1f ns   linearization %6.1f ns\n", name,
                t_eval / lanes, t_blocks / lanes, t_lin / lanes);
  }
};

int main() {
  Bench<double>::run("double (scalar)");
  Bench<Batch<double, 2>>::run("Batch<double,2>");
  Bench<Batch<double, 4>>::run("Batch<double,4>");
  Bench<Batch<double, 8>>::run("Batch<double,8>");
  Bench<float>::run("float (scalar)");
  Bench<Batch<float, 4>>::run("Batch<float,4>");
  Bench<Batch<float, 8>>::run("Batch<float,8>");
}
