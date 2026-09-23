#pragma once

// Argument types whose tangent spaces follow GTSAM's conventions, so that csym generates Jacobians
// GTSAM can use directly (no conversion at runtime). A residual function receives them as plain Pose2 /
// Pose3 (`presented_as`), so its semantics are unchanged; only the Lie traits used to define Jacobian
// columns differ:
//
//   GtsamPose2  tangent [vx, vy, w], retract(T, xi) = T * Exp(xi)   (SE(2) exponential, body frame)
//   GtsamPose3  tangent [w (3), v (3)], retract(T, xi) = T * Exp(xi) (SE(3) exponential, body frame)
//
// Only the derivative of retract at xi = 0 (storage_D_tangent) enters the generated code, so GTSAM's
// Expmap, Cayley and first-order retractions (which agree to first order) are all matched. Rot2, Rot3 and
// vectors already use GTSAM's conventions (Rot3: R * Exp(w), perturbation on the right).
//
// This header does not depend on GTSAM. (The standalone csym had a GTSAM adapter built on it; this tree
// solves with libs/factor_graph instead and did not bring it over.)

#include "csym/geo/pose2.h"
#include "csym/geo/pose3.h"
#include "csym/lie.h"

namespace csym {

template <class T>
struct GtsamPose2 : Pose2<T> {
  using Scalar = T;
  template <class S>
  using rebind = GtsamPose2<S>;
  using presented_as = Pose2<T>;  // what a residual function receives (see csym::present)

  constexpr GtsamPose2() = default;
  constexpr GtsamPose2(const Pose2<T>& p) : Pose2<T>(p) {}  // NOLINT: same value, GTSAM tangent

  static constexpr GtsamPose2 from_storage(const T* in) { return GtsamPose2(Pose2<T>::from_storage(in)); }
};

template <class T>
struct GtsamPose3 : Pose3<T> {
  using Scalar = T;
  template <class S>
  using rebind = GtsamPose3<S>;
  using presented_as = Pose3<T>;

  constexpr GtsamPose3() = default;
  constexpr GtsamPose3(const Pose3<T>& p) : Pose3<T>(p) {}  // NOLINT: same value, GTSAM tangent

  static constexpr GtsamPose3 from_storage(const T* in) { return GtsamPose3(Pose3<T>::from_storage(in)); }
};

template <class T>
struct lie<GtsamPose2<T>> {
  static constexpr std::size_t tangent_dim = 3;
  static constexpr bool identity_tangent = false;

  // d(T * Exp(xi))/d xi at 0, storage [c, s, x, y], tangent [vx, vy, w].
  static constexpr Matrix<T, 4, 3> storage_D_tangent(const GtsamPose2<T>& p) {
    const T c = p.R.re, s = p.R.im, o(0);
    return Matrix<T, 4, 3>{o, o, -s,  //
                           o, o, c,   //
                           c, -s, o,  //
                           s, c, o};
  }
  // SE(2) exponential (used for numerical checks; epsilon keeps w = 0 finite).
  static constexpr GtsamPose2<T> retract(const GtsamPose2<T>& p, const Vector3<T>& xi, const T& epsilon) {
    const T w = xi[2] + copysign_no_zero(epsilon, xi[2]);
    const T sw = sin(w) / w, cw = (T(1) - cos(w)) / w;
    const Vector2<T> t{sw * xi[0] - cw * xi[1], cw * xi[0] + sw * xi[1]};
    return GtsamPose2<T>(p * Pose2<T>(Rot2<T>::from_angle(xi[2]), t));
  }
};

template <class T>
struct lie<GtsamPose3<T>> {
  static constexpr std::size_t tangent_dim = 6;
  static constexpr bool identity_tangent = false;

  // d(T * Exp(xi))/d xi at 0 = blockdiag(d(q * exp(w))/dw, R), storage [q, t], tangent [w, v].
  static constexpr Matrix<T, 7, 6> storage_D_tangent(const GtsamPose3<T>& p) {
    Matrix<T, 7, 6> D = Matrix<T, 7, 6>::zero();
    D.set_block(0, 0, p.R.storage_D_tangent());
    D.set_block(4, 3, p.R.to_rotation_matrix());
    return D;
  }
  // SE(3) exponential (used for numerical checks).
  static constexpr GtsamPose3<T> retract(const GtsamPose3<T>& p, const Vector<T, 6>& xi, const T& epsilon) {
    const Vector3<T> w{xi[0], xi[1], xi[2]}, v{xi[3], xi[4], xi[5]};
    const T th2 = w.squared_norm() + epsilon * epsilon;
    const T th = sqrt(th2);
    const Matrix33<T> W = w.skew();
    const Matrix33<T> V = Matrix33<T>::identity() + W * ((T(1) - cos(th)) / th2) + (W * W) * ((th - sin(th)) / (th2 * th));
    return GtsamPose3<T>(p * Pose3<T>(Rot3<T>::from_tangent(w, epsilon), V * v));
  }
};

}  // namespace csym
