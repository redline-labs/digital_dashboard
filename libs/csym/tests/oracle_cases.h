#pragma once

// C++ twins of tools/symforce_oracle/cases.py. Keep the math, argument order and types in sync.

#include "vehicle_models.h"
#include "csym/csym.h"
#include "csym/noise_models.h"

namespace oracle_cases {

using namespace csym;

constexpr auto arith = [](auto x, auto y) {
  return x * y + x / y - 3 * x + y * y - pow(x - y, 3) / (1 + x * x);
};
constexpr auto trig = [](auto x, auto y) {
  using T = decltype(x);
  return Vector3<T>{sin(x) * cos(y), tan(x * y / 4), pow(sin(x + y), 2) - cos(2 * x)};
};
constexpr auto inverse_trig = [](auto x, auto y) {
  using T = decltype(x);
  return Vector3<T>{asin(x / 3), acos(y / 3), atan(x * y)};
};
constexpr auto atan2_eps = [](auto y, auto x, auto epsilon) { return atan2_safe(y, x, epsilon); };
constexpr auto exp_log = [](auto x, auto y) {
  using T = decltype(x);
  return Vector3<T>{exp(x) * log(y), tanh(x - y), log(1 + exp(-x * x))};
};
constexpr auto powers = [](auto x, auto y) {
  using T = decltype(x);
  return Vector<T, 4>{pow(x, y), pow(x, 3) - 2 * pow(x, -2), sqrt(y) + pow(y, -1.5), pow(x * y, 0.5)};
};
constexpr auto piecewise = [](auto x, auto y) {
  using T = decltype(x);
  return Vector<T, 4>{min(x, y) * max(x, 2 * y), abs(x - y) * x, sign(x) * y * y, max(x * y, T(0))};
};
constexpr auto nested = [](auto x, auto y, auto epsilon) {
  return sin(exp(x / 2) + sqrt(x * x + y * y + epsilon * epsilon)) / (2 + cos(x * y));
};
constexpr auto norm_eps = [](auto v, auto epsilon) { return v / v.norm(epsilon); };

constexpr auto rot3_act = [](auto R, auto p) { return R * p; };
constexpr auto rot3_compose = [](auto a, auto b) { return (a * b).to_rotation_matrix(); };
constexpr auto rot3_local = [](auto a, auto b, auto epsilon) { return a.local_coordinates(b, epsilon); };
constexpr auto rot3_from_tangent = [](auto v, auto epsilon) {
  using T = decltype(epsilon);
  return Rot3<T>::from_tangent(v, epsilon).to_rotation_matrix();
};
constexpr auto rot2_local = [](auto a, auto b, auto epsilon) { return a.local_coordinates(b, epsilon); };
constexpr auto pose2_between = [](auto a, auto b, auto a_T_b, auto sqrt_info, auto epsilon) {
  return between_residual(a, b, a_T_b, sqrt_info, epsilon);
};
constexpr auto pose3_between = [](auto a, auto b, auto a_T_b, auto sqrt_info, auto epsilon) {
  return between_residual(a, b, a_T_b, sqrt_info, epsilon);
};
constexpr auto pose3_prior = [](auto x, auto prior, auto sqrt_info, auto epsilon) {
  return prior_residual(x, prior, sqrt_info, epsilon);
};
constexpr auto pose3_inverse_act = [](auto T_, auto p) { return T_.inverse() * p; };

constexpr auto rot3_compose_group = [](auto a, auto b) { return a * b; };
constexpr auto pose3_compose_group = [](auto a, auto b) { return a * b; };
constexpr auto pose2_between_group = [](auto a, auto b) { return a.between(b); };

constexpr auto barron_whiten = [](auto v, auto alpha, auto s, auto epsilon) {
  using T = decltype(alpha);
  return BarronNoiseModel<T>{alpha, s, epsilon}.whiten(v);
};
constexpr auto barron_whiten_norm = [](auto v, auto alpha, auto s, auto delta, auto epsilon) {
  using T = decltype(alpha);
  return BarronNoiseModel<T>{alpha, s, epsilon, delta}.whiten_norm(v, epsilon);
};
constexpr auto pseudo_huber_whiten_norm = [](auto v, auto delta, auto s, auto epsilon) {
  using T = decltype(delta);
  return PseudoHuberNoiseModel<T>{delta, s, epsilon}.whiten_norm(v, epsilon);
};
constexpr auto robust_pose2_between = [](auto a, auto b, auto a_T_b, auto sigmas, auto epsilon) {
  using T = decltype(epsilon);
  const Vector3<T> r = a_T_b.local_coordinates(a.between(b), epsilon);
  const Vector3<T> w{r[0] / sigmas[0], r[1] / sigmas[1], r[2] / sigmas[2]};
  return BarronNoiseModel<T>::cauchy(T(1), epsilon).whiten_norm(w, epsilon);
};

using vehicle::bicycle_factor;
using vehicle::robust_tire_factor;
using vehicle::tire_fy;

}  // namespace oracle_cases
