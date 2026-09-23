#pragma once

// SE(2) pose stored as [re, im, tx, ty]. Tangent: [θ, x, y].
//
// retract/local_coordinates treat rotation and translation separately (the product manifold
// SO(2) x R²): retract(T, [δθ, δt]) = (R ⊗ exp(δθ), t + δt). This matches SymForce's convention and
// keeps the translation Jacobian blocks trivial.

#include "csym/geo/rot2.h"

namespace csym {

template <class T>
class Pose2 {
 public:
  using Scalar = T;
  template <class S>
  using rebind = Pose2<S>;
  static constexpr std::size_t storage_dim = 4;
  static constexpr std::size_t tangent_dim = 3;
  static constexpr bool identity_tangent = false;

  Rot2<T> R;
  Vector2<T> t = Vector2<T>::zero();

  constexpr Pose2() = default;
  constexpr Pose2(const Rot2<T>& r, const Vector2<T>& p) : R(r), t(p) {}

  static constexpr Pose2 identity() { return {Rot2<T>::identity(), Vector2<T>::zero()}; }
  static constexpr Pose2 from_angle_position(const T& theta, const T& x, const T& y) {
    return {Rot2<T>::from_angle(theta), Vector2<T>{x, y}};
  }

  constexpr Pose2 operator*(const Pose2& b) const { return {R * b.R, t + R * b.t}; }
  constexpr Vector2<T> operator*(const Vector2<T>& p) const { return R * p + t; }
  constexpr Pose2 compose(const Pose2& b) const { return *this * b; }
  constexpr Pose2 inverse() const {
    const Rot2<T> ri = R.inverse();
    return {ri, -(ri * t)};
  }
  constexpr Pose2 between(const Pose2& b) const { return inverse() * b; }
  constexpr T angle(const T& epsilon = T(0)) const { return R.angle(epsilon); }

  constexpr Pose2 retract(const Vector3<T>& d, const T& epsilon = T(0)) const {
    Vector<T, 1> dr;
    dr[0] = d[0];
    return {R.retract(dr, epsilon), t + Vector2<T>{d[1], d[2]}};
  }
  constexpr Vector3<T> local_coordinates(const Pose2& b, const T& epsilon = T(0)) const {
    const T dth = R.local_coordinates(b.R, epsilon)[0];
    return Vector3<T>{dth, b.t[0] - t[0], b.t[1] - t[1]};
  }
  constexpr Matrix<T, 4, 3> storage_D_tangent() const {
    const T o(0), l(1);
    return Matrix<T, 4, 3>{-R.im, o, o,  //
                           R.re,  o, o,  //
                           o,     l, o,  //
                           o,     o, l};
  }

  constexpr Matrix<T, 3, 4> tangent_D_storage() const {
    const T o(0), l(1);
    return Matrix<T, 3, 4>{-R.im, R.re, o, o,  //
                           o,     o,    l, o,  //
                           o,     o,    o, l};
  }

  constexpr void to_storage(T* out) const {
    R.to_storage(out);
    out[2] = t[0];
    out[3] = t[1];
  }
  static constexpr Pose2 from_storage(const T* in) { return {Rot2<T>::from_storage(in), Vector2<T>{in[2], in[3]}}; }
};

}  // namespace csym
