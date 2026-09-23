#pragma once

// SO(3) stored as a unit quaternion [x, y, z, w] (Hamilton convention). Tangent: rotation vector ω,
// with retract(q, ω) = q ⊗ exp(ω) (perturbation on the right).

#include "csym/matrix.h"

namespace csym {

template <class T>
class Rot3 {
 public:
  using Scalar = T;
  template <class S>
  using rebind = Rot3<S>;
  static constexpr std::size_t storage_dim = 4;
  static constexpr std::size_t tangent_dim = 3;
  static constexpr bool identity_tangent = false;

  T x{0}, y{0}, z{0}, w{1};

  constexpr Rot3() = default;
  constexpr Rot3(const T& qx, const T& qy, const T& qz, const T& qw) : x(qx), y(qy), z(qz), w(qw) {}

  static constexpr Rot3 identity() { return {T(0), T(0), T(0), T(1)}; }

  // exp: ω -> quaternion. epsilon keeps the expression finite and differentiable at ω = 0.
  static constexpr Rot3 from_tangent(const Vector3<T>& v, const T& epsilon = T(0)) {
    const T theta = sqrt(v.squared_norm() + epsilon * epsilon);
    const T s = sin(theta / T(2)) / theta;
    return {v[0] * s, v[1] * s, v[2] * s, cos(theta / T(2))};
  }
  // log: quaternion -> ω. Chooses the representative with w >= 0 (shortest rotation).
  constexpr Vector3<T> to_tangent(const T& epsilon = T(0)) const {
    const T wc = min(abs(w), T(1) - epsilon);
    const T scale = T(2) * sign_no_zero(w) * acos(wc) / sqrt(T(1) - wc * wc);
    return Vector3<T>{x * scale, y * scale, z * scale};
  }

  // Rotation about unit axis by angle.
  static constexpr Rot3 from_angle_axis(const T& angle, const Vector3<T>& axis) {
    const T s = sin(angle / T(2));
    return {axis[0] * s, axis[1] * s, axis[2] * s, cos(angle / T(2))};
  }
  // R = Rz(yaw) * Ry(pitch) * Rx(roll)
  static constexpr Rot3 from_yaw_pitch_roll(const T& yaw, const T& pitch, const T& roll) {
    const Rot3 rz = from_angle_axis(yaw, Vector3<T>{T(0), T(0), T(1)});
    const Rot3 ry = from_angle_axis(pitch, Vector3<T>{T(0), T(1), T(0)});
    const Rot3 rx = from_angle_axis(roll, Vector3<T>{T(1), T(0), T(0)});
    return rz * ry * rx;
  }

  constexpr Rot3 operator*(const Rot3& b) const {
    return {w * b.x + x * b.w + y * b.z - z * b.y,  //
            w * b.y - x * b.z + y * b.w + z * b.x,  //
            w * b.z + x * b.y - y * b.x + z * b.w,  //
            w * b.w - x * b.x - y * b.y - z * b.z};
  }
  constexpr Vector3<T> operator*(const Vector3<T>& p) const { return to_rotation_matrix() * p; }
  constexpr Rot3 compose(const Rot3& b) const { return *this * b; }
  constexpr Rot3 inverse() const { return {-x, -y, -z, w}; }
  constexpr Rot3 between(const Rot3& b) const { return inverse() * b; }

  constexpr Matrix33<T> to_rotation_matrix() const {
    const T xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z;
    const T wx = w * x, wy = w * y, wz = w * z;
    return Matrix33<T>{T(1) - T(2) * (yy + zz), T(2) * (xy - wz),         T(2) * (xz + wy),
                       T(2) * (xy + wz),         T(1) - T(2) * (xx + zz), T(2) * (yz - wx),
                       T(2) * (xz - wy),         T(2) * (yz + wx),         T(1) - T(2) * (xx + yy)};
  }

  constexpr Rot3 retract(const Vector3<T>& d, const T& epsilon = T(0)) const {
    return *this * from_tangent(d, epsilon);
  }
  constexpr Vector3<T> local_coordinates(const Rot3& b, const T& epsilon = T(0)) const {
    return between(b).to_tangent(epsilon);
  }
  // d(q ⊗ exp(δ))/dδ at δ = 0 = ½ [w I + [q_v]×; -q_vᵀ]
  constexpr Matrix<T, 4, 3> storage_D_tangent() const {
    const T h(0.5);
    return Matrix<T, 4, 3>{h * w,  -h * z, h * y,   //
                           h * z,  h * w,  -h * x,  //
                           -h * y, h * x,  h * w,   //
                           -h * x, -h * y, -h * z};
  }

  // d(local_coordinates(this, y))/d(storage of y) at y = this: 2 * vec(q* ⊗ dy) (unit quaternion).
  constexpr Matrix<T, 3, 4> tangent_D_storage() const {
    const T t(2);
    return Matrix<T, 3, 4>{t * w,  t * z,  -t * y, -t * x,  //
                           -t * z, t * w,  t * x,  -t * y,  //
                           t * y,  -t * x, t * w,  -t * z};
  }

  constexpr void to_storage(T* out) const {
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
  }
  static constexpr Rot3 from_storage(const T* in) { return {in[0], in[1], in[2], in[3]}; }
};

}  // namespace csym
