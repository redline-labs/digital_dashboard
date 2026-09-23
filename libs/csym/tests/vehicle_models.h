#pragma once

// Vehicle dynamics and tire models written once, generically, so they can be compiled by csym into
// residuals with Jacobians (T = csym::Expr during tracing) or evaluated directly on doubles.
//
// Each model is a constexpr generic lambda; wrap it in csym::Function<model, ArgTypes...> to get
// compile-time generated eval / jacobian / linearize.

#include "csym/csym.h"
#include "csym/noise_models.h"

namespace vehicle {

using namespace csym;  // math functions resolve for both double and Expr

// Simplified Pacejka "Magic Formula" lateral force: F = D sin(C atan(Bα − E(Bα − atan(Bα)))).
template <class T>
constexpr T pacejka(const T& alpha, const T& B, const T& C, const T& D, const T& E) {
  const T x = B * alpha;
  return D * sin(C * atan(x - E * (x - atan(x))));
}

// Slip angle of a wheel at longitudinal offset `a` from the CG, steered by `delta`: the angle from the
// wheel's velocity to its heading, alpha = delta - atan2(vy + a r, vx). With this sign convention the
// Pacejka force below is restoring (positive alpha -> positive lateral force).
template <class T>
constexpr T slip_angle(const T& vx, const T& vy, const T& r, const T& a, const T& delta, const T& epsilon) {
  return delta - atan2_safe(vy + a * r, vx, epsilon);
}

// Front tire lateral force from vehicle states: args (vx, vy, r, delta, a, B, C, mu, Fz, E, epsilon).
constexpr auto tire_fy = [](auto vx, auto vy, auto r, auto delta, auto a, auto B, auto C, auto mu, auto Fz,
                            auto E, auto epsilon) {
  return pacejka(slip_angle(vx, vy, r, a, delta, epsilon), B, C, mu * Fz, E);
};

// Lateral-force measurement factor (e.g. from a wheel force transducer or an inverse-dynamics estimate):
// the whitened error between measured and modeled force under an adaptive Barron loss. With alpha as a
// variable the smoother can estimate how heavy-tailed the force errors are; fix it (e.g. 0 for Cauchy)
// for a plain robust factor. Args: (vx, vy, r, delta, a, B, C, mu, Fz, E, fy_measured, sigma, alpha, eps).
constexpr auto robust_tire_factor = [](auto vx, auto vy, auto r, auto delta, auto a, auto B, auto C, auto mu,
                                       auto Fz, auto E, auto fy_measured, auto sigma, auto alpha, auto epsilon) {
  using T = decltype(vx);
  const T fy = tire_fy(vx, vy, r, delta, a, B, C, mu, Fz, E, epsilon);
  Vector<T, 1> w;
  w[0] = (fy - fy_measured) / sigma;
  return BarronNoiseModel<T>{alpha, T(1), epsilon}.whiten_norm(w, epsilon);
};

// Planar dynamic bicycle model, as a smoother factor between consecutive states (explicit Euler).
//
//   state:   pose (Pose2, world frame), vel = [vx, vy, r] (body frame)
//   input:   u = [delta (steer), Fx (longitudinal force)]
//   params:  chassis = [m, Iz, lf, lr], tire_f / tire_r = Pacejka [B, C, D, E]
//   residual (6): [pose_pred ⊖ pose_k1 (3); vel_k1 - vel_pred (3)] scaled by sqrt_info_diag
template <class T>
struct BicycleDerivatives {
  T vx_dot, vy_dot, r_dot;
};

template <class T>
constexpr BicycleDerivatives<T> bicycle_dynamics(const Vector3<T>& vel, const Vector2<T>& u,
                                                 const Vector<T, 4>& chassis, const Vector<T, 4>& tire_f,
                                                 const Vector<T, 4>& tire_r, const T& epsilon) {
  const T vx = vel[0], vy = vel[1], r = vel[2];
  const T delta = u[0], Fx = u[1];
  const T m = chassis[0], Iz = chassis[1], lf = chassis[2], lr = chassis[3];
  const T alpha_f = slip_angle(vx, vy, r, lf, delta, epsilon);
  const T alpha_r = slip_angle(vx, vy, r, -lr, T(0), epsilon);
  const T Fyf = pacejka(alpha_f, tire_f[0], tire_f[1], tire_f[2], tire_f[3]);
  const T Fyr = pacejka(alpha_r, tire_r[0], tire_r[1], tire_r[2], tire_r[3]);
  return {(Fx - Fyf * sin(delta)) / m + vy * r,  //
          (Fyr + Fyf * cos(delta)) / m - vx * r,  //
          (lf * Fyf * cos(delta) - lr * Fyr) / Iz};
}

constexpr auto bicycle_factor = [](auto pose_k, auto vel_k, auto pose_k1, auto vel_k1, auto u, auto chassis,
                                   auto tire_f, auto tire_r, auto dt, auto sqrt_info_diag, auto epsilon) {
  using T = decltype(dt);
  const T vx = vel_k[0], vy = vel_k[1], r = vel_k[2];
  const auto d = bicycle_dynamics(vel_k, u, chassis, tire_f, tire_r, epsilon);
  const Pose2<T> step{Rot2<T>::from_angle(r * dt), Vector2<T>{vx * dt, vy * dt}};
  const Pose2<T> pose_pred = pose_k * step;
  const Vector3<T> vel_pred{vx + d.vx_dot * dt, vy + d.vy_dot * dt, r + d.r_dot * dt};
  const Vector3<T> res_pose = pose_pred.local_coordinates(pose_k1, epsilon);
  const Vector3<T> res_vel = vel_k1 - vel_pred;
  Vector<T, 6> res;
  for (std::size_t i = 0; i < 3; ++i) {
    res[i] = res_pose[i] * sqrt_info_diag[i];
    res[3 + i] = res_vel[i] * sqrt_info_diag[3 + i];
  }
  return res;
};

// Same factor with a classical RK4 step over the full planar state [x, y, ψ, vx, vy, r] (world-frame
// position, heading, body-frame velocities), holding u constant over the step.
template <class T>
constexpr Vector<T, 6> bicycle_state_derivative(const Vector<T, 6>& s, const Vector2<T>& u,
                                                const Vector<T, 4>& chassis, const Vector<T, 4>& tire_f,
                                                const Vector<T, 4>& tire_r, const T& epsilon) {
  const T psi = s[2], vx = s[3], vy = s[4], r = s[5];
  const auto d = bicycle_dynamics(Vector3<T>{vx, vy, r}, u, chassis, tire_f, tire_r, epsilon);
  const T c = cos(psi), sn = sin(psi);
  return Vector<T, 6>{c * vx - sn * vy, sn * vx + c * vy, r, d.vx_dot, d.vy_dot, d.r_dot};
}

constexpr auto bicycle_factor_rk4 = [](auto pose_k, auto vel_k, auto pose_k1, auto vel_k1, auto u, auto chassis,
                                       auto tire_f, auto tire_r, auto dt, auto sqrt_info_diag, auto epsilon) {
  using T = decltype(dt);
  const Vector<T, 6> s0{pose_k.t[0], pose_k.t[1], pose_k.angle(epsilon), vel_k[0], vel_k[1], vel_k[2]};
  auto f = [&](const Vector<T, 6>& s) { return bicycle_state_derivative(s, u, chassis, tire_f, tire_r, epsilon); };
  const Vector<T, 6> k1 = f(s0);
  const Vector<T, 6> k2 = f(s0 + k1 * (dt / T(2)));
  const Vector<T, 6> k3 = f(s0 + k2 * (dt / T(2)));
  const Vector<T, 6> k4 = f(s0 + k3 * dt);
  const Vector<T, 6> s1 = s0 + (k1 + k2 * T(2) + k3 * T(2) + k4) * (dt / T(6));
  const Pose2<T> pose_pred = Pose2<T>::from_angle_position(s1[2], s1[0], s1[1]);
  const Vector3<T> res_pose = pose_pred.local_coordinates(pose_k1, epsilon);
  Vector<T, 6> res;
  for (std::size_t i = 0; i < 3; ++i) {
    res[i] = res_pose[i] * sqrt_info_diag[i];
    res[3 + i] = (vel_k1[i] - s1[3 + i]) * sqrt_info_diag[3 + i];
  }
  return res;
};

}  // namespace vehicle
