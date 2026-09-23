#include "geo_check.h"

using namespace csym;
using geo_check::check_function;

constexpr auto rot3_act = [](auto R, auto p, auto) { return R * p; };
constexpr auto rot3_compose = [](auto a, auto b, auto) { return (a * b).to_rotation_matrix(); };
constexpr auto rot3_local = [](auto a, auto b, auto eps) { return local_coordinates(a, b, eps); };
constexpr auto rot3_from_tangent = [](auto v, auto eps) {
  using T = decltype(eps);
  return Rot3<T>::from_tangent(v, eps).to_rotation_matrix();
};

int main() {
  const double eps = 1e-12;
  const auto Ra = Rot3<double>::from_yaw_pitch_roll(0.3, -0.2, 0.7);
  const auto Rb = Rot3<double>::from_yaw_pitch_roll(-0.5, 0.4, 0.1);
  const Vector3<double> p{1.0, -2.0, 0.5};
  check_function<rot3_act>("rot3 * point", Ra, p, eps);
  check_function<rot3_compose>("rot3 compose", Ra, Rb, eps);
  check_function<rot3_local>("rot3 local_coords", Ra, Rb, eps);
  check_function<rot3_from_tangent>("rot3 from_tangent", Vector3<double>{0.1, -0.3, 0.2}, eps);
  return check::report("geo_rot3");
}
