#pragma once

// Noise models: whitening and robust losses applied *inside* the residual function, so the generated
// Jacobians include the loss exactly (not an iteratively-reweighted approximation). Conventions follow
// SymForce's noise models:
//
//   whiten(v)       per element: Gaussian models scale; robust models map each element to
//                   sqrt(eps + 2 rho(v_i)) - sqrt(eps) (non-negative), so 0.5 |whiten(v)|^2 = sum rho(v_i)
//                   up to eps.
//   whiten_norm(v)  robust in the vector's norm, direction kept: v * sqrt(2 rho(n)) / n with
//                   n = sqrt(eps + |v|^2), so 0.5 |whiten_norm(v)|^2 = rho(|v|). Use it for a factor whose
//                   residual block should be down-weighted as a whole (e.g. a tire or pose outlier).
//   error(v)        0.5 |whiten(v)|^2
//
// All types are generic over the scalar (double, float, Batch, Expr), so the same code works numerically
// and when traced by csym::Function. Example (robust tire factor):
//
//   constexpr auto f = [](auto fy_measured, ..., auto alpha, auto eps) {
//     using T = decltype(eps);
//     Vector<T, 1> r{(fy_model - fy_measured) / sigma};
//     return BarronNoiseModel<T>{alpha, T(1), eps}.whiten_norm(r, eps);
//   };

#include <cstddef>

#include "csym/core/expr.h"
#include "csym/matrix.h"

namespace csym {

// ---- Gaussian ---------------------------------------------------------------------------------------

// sqrt_information * v with a scalar sqrt information (sigma^-1).
template <class T>
struct IsotropicNoiseModel {
  T sqrt_information;

  static constexpr IsotropicNoiseModel from_sigma(const T& sigma) { return {T(1) / sigma}; }
  static constexpr IsotropicNoiseModel from_variance(const T& variance) { return {T(1) / sqrt(variance)}; }
  static constexpr IsotropicNoiseModel from_information(const T& information) { return {sqrt(information)}; }

  template <std::size_t N>
  constexpr Vector<T, N> whiten(const Vector<T, N>& v) const {
    return v * sqrt_information;
  }
  template <std::size_t N>
  constexpr T error(const Vector<T, N>& v) const {
    return T(0.5) * whiten(v).squared_norm();
  }
};

// Per-element sqrt information (sigma_i^-1).
template <class T, std::size_t N>
struct DiagonalNoiseModel {
  Vector<T, N> sqrt_information;

  static constexpr DiagonalNoiseModel from_sigmas(const Vector<T, N>& sigmas) {
    Vector<T, N> s;
    for (std::size_t i = 0; i < N; ++i) s[i] = T(1) / sigmas[i];
    return {s};
  }
  static constexpr DiagonalNoiseModel from_variances(const Vector<T, N>& variances) {
    Vector<T, N> s;
    for (std::size_t i = 0; i < N; ++i) s[i] = T(1) / sqrt(variances[i]);
    return {s};
  }
  constexpr Vector<T, N> whiten(const Vector<T, N>& v) const {
    Vector<T, N> r;
    for (std::size_t i = 0; i < N; ++i) r[i] = v[i] * sqrt_information[i];
    return r;
  }
  constexpr T error(const Vector<T, N>& v) const { return T(0.5) * whiten(v).squared_norm(); }
};

// Full square-root information matrix (e.g. the Cholesky factor of an information matrix).
template <class T, std::size_t N>
struct SqrtInformationNoiseModel {
  Matrix<T, N, N> sqrt_information;

  constexpr Vector<T, N> whiten(const Vector<T, N>& v) const { return sqrt_information * v; }
  constexpr T error(const Vector<T, N>& v) const { return T(0.5) * whiten(v).squared_norm(); }
};

// ---- robust ---------------------------------------------------------------------------------------------

namespace detail {
// Robust whitening shared by the scalar-loss models; Derived provides rho(x).
template <class Derived, class T>
struct RobustNoiseModel {
  constexpr const Derived& self() const { return static_cast<const Derived&>(*this); }

  template <std::size_t N>
  constexpr Vector<T, N> whiten(const Vector<T, N>& v) const {
    const T eps = self().epsilon();
    Vector<T, N> r;
    for (std::size_t i = 0; i < N; ++i) r[i] = sqrt(eps + T(2) * self().rho(v[i])) - sqrt(eps);
    return r;
  }
  template <std::size_t N>
  constexpr Vector<T, N> whiten_norm(const Vector<T, N>& v, const T& epsilon) const {
    const T n = sqrt(epsilon + v.squared_norm());
    return v * (sqrt(T(2) * self().rho(n)) / n);
  }
  template <std::size_t N>
  constexpr T error(const Vector<T, N>& v) const {
    return T(0.5) * whiten(v).squared_norm();
  }
};
}  // namespace detail

// Barron's general and adaptive robust loss ("A General and Adaptive Robust Loss Function", CVPR 2019):
//
//   rho(x) = delta^2 * b/d * ((1 + s x^2 / (delta^2 b))^(d/2) - 1),
//   b = |alpha - 2| + alpha_epsilon,  d = alpha + alpha_epsilon * sign_no_zero(alpha),
//
// with s the scalar information and delta the transition point. alpha selects the shape (2: L2,
// 1: pseudo-Huber/Charbonnier, 0: Cauchy, -2: Geman-McClure, -inf: Welsch); alpha_epsilon keeps
// alpha = 0 and alpha = 2 finite. alpha can be a variable, e.g. to estimate it.
template <class T>
struct BarronNoiseModel : detail::RobustNoiseModel<BarronNoiseModel<T>, T> {
  T alpha, scalar_information, x_epsilon, delta, alpha_epsilon;

  constexpr BarronNoiseModel(const T& alpha_, const T& scalar_information_, const T& x_epsilon_,
                             const T& delta_ = T(1))
      : BarronNoiseModel(alpha_, scalar_information_, x_epsilon_, delta_, x_epsilon_) {}
  constexpr BarronNoiseModel(const T& alpha_, const T& scalar_information_, const T& x_epsilon_, const T& delta_,
                             const T& alpha_epsilon_)
      : alpha(alpha_),
        scalar_information(scalar_information_),
        x_epsilon(x_epsilon_),
        delta(delta_),
        alpha_epsilon(alpha_epsilon_) {}

  static constexpr BarronNoiseModel cauchy(const T& scalar_information, const T& epsilon, const T& delta = T(1)) {
    return {T(0), scalar_information, epsilon, delta};
  }
  static constexpr BarronNoiseModel geman_mcclure(const T& scalar_information, const T& epsilon,
                                                  const T& delta = T(1)) {
    return {T(-2), scalar_information, epsilon, delta};
  }
  static constexpr BarronNoiseModel charbonnier(const T& scalar_information, const T& epsilon, const T& delta = T(1)) {
    return {T(1), scalar_information, epsilon, delta};
  }

  constexpr T epsilon() const { return x_epsilon; }
  constexpr T rho(const T& x) const {
    const T b = abs(alpha - T(2)) + alpha_epsilon;
    const T d = alpha + alpha_epsilon * sign_no_zero(alpha);
    const T d2 = delta * delta;
    return d2 * b / d * (pow(T(1) + scalar_information * x * x / (d2 * b), d / T(2)) - T(1));
  }
};

// Pseudo-Huber: rho(x) = delta^2 (sqrt(1 + s x^2 / delta^2) - 1). Quadratic near 0, linear beyond delta.
template <class T>
struct PseudoHuberNoiseModel : detail::RobustNoiseModel<PseudoHuberNoiseModel<T>, T> {
  T delta, scalar_information, eps;

  constexpr PseudoHuberNoiseModel(const T& delta_, const T& scalar_information_, const T& epsilon_)
      : delta(delta_), scalar_information(scalar_information_), eps(epsilon_) {}

  constexpr T epsilon() const { return eps; }
  // delta^2 (sqrt(1 + u) - 1), u = s x^2 / delta^2, written as s x^2 / (sqrt(1 + u) + 1): the same
  // number, without the cancellation that makes the first form exactly zero once u is below machine
  // epsilon (a large delta, or a residual near zero, where its derivative still matters).
  constexpr T rho(const T& x) const {
    const T sx2 = scalar_information * x * x;
    return sx2 / (sqrt(T(1) + sx2 / (delta * delta)) + T(1));
  }
};

}  // namespace csym
