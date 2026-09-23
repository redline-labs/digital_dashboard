#pragma once

// SE(3) pose stored as [qx, qy, qz, qw, tx, ty, tz]. Tangent: [ωx, ωy, ωz, x, y, z].
//
// retract/local_coordinates treat rotation and translation separately (the product manifold
// SO(3) x R³): retract(T, [ω, δt]) = (R ⊗ exp(ω), t + δt). This matches SymForce's convention.

#include "csym/geo/rot3.h"

namespace csym {

template <class T>
class Pose3 {
 public:
  using Scalar = T;
  template <class S>
  using rebind = Pose3<S>;
  static constexpr std::size_t storage_dim = 7;
  static constexpr std::size_t tangent_dim = 6;
  static constexpr bool identity_tangent = false;

  Rot3<T> R;
  Vector3<T> t = Vector3<T>::zero();

  constexpr Pose3() = default;
  constexpr Pose3(const Rot3<T>& r, const Vector3<T>& p) : R(r), t(p) {}

  static constexpr Pose3 identity() { return {Rot3<T>::identity(), Vector3<T>::zero()}; }

  constexpr Pose3 operator*(const Pose3& b) const { return {R * b.R, t + R * b.t}; }
  constexpr Vector3<T> operator*(const Vector3<T>& p) const { return R * p + t; }
  constexpr Pose3 compose(const Pose3& b) const { return *this * b; }
  constexpr Pose3 inverse() const {
    const Rot3<T> ri = R.inverse();
    return {ri, -(ri * t)};
  }
  constexpr Pose3 between(const Pose3& b) const { return inverse() * b; }

  constexpr Pose3 retract(const Vector<T, 6>& d, const T& epsilon = T(0)) const {
    return {R.retract(Vector3<T>{d[0], d[1], d[2]}, epsilon), t + Vector3<T>{d[3], d[4], d[5]}};
  }
  constexpr Vector<T, 6> local_coordinates(const Pose3& b, const T& epsilon = T(0)) const {
    const Vector3<T> w = R.local_coordinates(b.R, epsilon);
    return Vector<T, 6>{w[0], w[1], w[2], b.t[0] - t[0], b.t[1] - t[1], b.t[2] - t[2]};
  }
  constexpr Matrix<T, 7, 6> storage_D_tangent() const {
    Matrix<T, 7, 6> D = Matrix<T, 7, 6>::zero();
    D.set_block(0, 0, R.storage_D_tangent());
    D.set_block(4, 3, Matrix33<T>::identity());
    return D;
  }

  constexpr Matrix<T, 6, 7> tangent_D_storage() const {
    Matrix<T, 6, 7> D = Matrix<T, 6, 7>::zero();
    D.set_block(0, 0, R.tangent_D_storage());
    D.set_block(3, 4, Matrix33<T>::identity());
    return D;
  }

  constexpr void to_storage(T* out) const {
    R.to_storage(out);
    for (std::size_t i = 0; i < 3; ++i) out[4 + i] = t[i];
  }
  static constexpr Pose3 from_storage(const T* in) {
    return {Rot3<T>::from_storage(in), Vector3<T>{in[4], in[5], in[6]}};
  }
};

}  // namespace csym
