#include "geo_check.h"

using namespace csym;
using geo_check::check_function;

constexpr auto pose3_between = [](auto a, auto b, auto a_T_b, auto sqrt_info, auto eps) {
  return between_residual(a, b, a_T_b, sqrt_info, eps);
};
constexpr auto pose3_prior = [](auto x, auto prior, auto sqrt_info, auto eps) {
  return prior_residual(x, prior, sqrt_info, eps);
};
constexpr auto pose3_transform = [](auto T, auto p, auto) { return T.inverse() * p; };

int main() {
  const double eps = 1e-12;
  const auto Ra = Rot3<double>::from_yaw_pitch_roll(0.3, -0.2, 0.7);
  const auto Rb = Rot3<double>::from_yaw_pitch_roll(-0.5, 0.4, 0.1);
  const Vector3<double> p{1.0, -2.0, 0.5};
  const Pose3<double> Qa{Ra, Vector3<double>{1, 2, 3}}, Qb{Rb, Vector3<double>{1.5, 1.8, 3.4}};
  const Pose3<double> Qab{Rot3<double>::from_yaw_pitch_roll(-0.7, 0.5, -0.5), Vector3<double>{0.3, -0.2, 0.4}};
  auto I6 = Matrix<double, 6, 6>::identity();
  I6(0, 1) = 0.3;
  I6(4, 4) = 2.0;
  check_function<pose3_between>("pose3 between", Qa, Qb, Qab, I6, eps);
  check_function<pose3_prior>("pose3 prior", Qa, Qb, I6, eps);
  check_function<pose3_transform>("pose3 inverse * point", Qa, p, eps);
  return check::report("geo_pose3");
}
