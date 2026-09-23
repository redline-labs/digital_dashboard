#include <cmath>

#include "check.h"
#include "csym/csym.h"

using csym::Vector3;

using csym::Function;

constexpr auto f1 = [](auto x, auto y) { return x * sin(y) + 2.0 * x * x; };
using F1 = Function<f1, double, double>;

// compile-time evaluation
static_assert(F1::eval(2.0, 0.0) == 8.0);
static_assert(F1::jacobian(2.0, 0.0).jacobian(0, 0) == 8.0);  // d/dx = sin(y) + 4x
static_assert(F1::jacobian(2.0, 0.0).jacobian(0, 1) == 2.0);  // d/dy = x cos(y)

// Multiple outputs: std::tuple and std::array of storable types.
constexpr auto multi = [](auto x, auto R) {
  using T = decltype(x);
  return std::tuple{x * x, R * Vector3<T>{x, T(1), T(0)}, std::array<T, 2>{sin(x), cos(x)}};
};
using Multi = csym::Function<multi, double, csym::Rot3<double>>;
static_assert(Multi::output_dim == 1 + 3 + 2);

int main() {
  const double x = 1.3, y = 0.7;
  CHECK_NEAR(F1::eval(x, y), x * std::sin(y) + 2 * x * x, 1e-15);
  const auto [v, J] = F1::jacobian(x, y);
  CHECK_NEAR(v, x * std::sin(y) + 2 * x * x, 1e-15);
  CHECK_NEAR(J(0, 0), std::sin(y) + 4 * x, 1e-15);
  CHECK_NEAR(J(0, 1), x * std::cos(y), 1e-15);
  {
    const auto R = csym::Rot3<double>::from_yaw_pitch_roll(0.3, 0.0, 0.0);
    const auto [mv, MJ] = Multi::jacobian(0.5, R);
    CHECK_NEAR(std::get<0>(mv), 0.25, 1e-15);
    CHECK_NEAR(std::get<1>(mv)[0], std::cos(0.3) * 0.5 - std::sin(0.3), 1e-15);
    CHECK_NEAR(std::get<2>(mv)[1], std::cos(0.5), 1e-15);
    CHECK_NEAR(MJ(0, 0), 1.0, 1e-15);           // d(x^2)/dx
    CHECK_NEAR(MJ(5, 0), -std::sin(0.5), 1e-15);  // d(cos x)/dx
    CHECK(MJ.cols == 4);                          // x + Rot3 tangent (3)
  }
  std::printf("ops value=%zu jacobian=%zu\n", F1::op_count<csym::Mode::Value>,
              F1::op_count<csym::Mode::Jacobian>);
  return check::report("smoke");
}
