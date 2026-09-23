#include "geo_check.h"

using namespace csym;
using geo_check::check_function;

constexpr auto rot2_between = [](auto a, auto b, auto eps) { return local_coordinates(a, b, eps); };
constexpr auto pose2_between = [](auto a, auto b, auto a_T_b, auto sqrt_info, auto eps) {
  return between_residual(a, b, a_T_b, sqrt_info, eps);
};

int main() {
  const double eps = 1e-12;
  const auto r2a = Rot2<double>::from_angle(0.4), r2b = Rot2<double>::from_angle(-1.1);
  check_function<rot2_between>("rot2 local_coords", r2a, r2b, eps);

  const auto Pa = Pose2<double>::from_angle_position(0.3, 1.0, 2.0);
  const auto Pb = Pose2<double>::from_angle_position(-0.2, 1.5, 2.7);
  const auto Pab = Pose2<double>::from_angle_position(-0.45, 0.4, 0.8);
  const auto I3 = Matrix33<double>{2.0, 0.1, 0.0, 0.0, 3.0, 0.2, 0.0, 0.0, 1.5};
  check_function<pose2_between>("pose2 between", Pa, Pb, Pab, I3, eps);
  return check::report("geo_pose2");
}
