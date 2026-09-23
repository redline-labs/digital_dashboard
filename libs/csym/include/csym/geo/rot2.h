#pragma once

// SO(2) stored as a unit complex number [re, im] = [cos θ, sin θ]. Tangent: θ.

#include "csym/matrix.h"

namespace csym {

template <class T>
class Rot2 {
 public:
  using Scalar = T;
  template <class S>
  using rebind = Rot2<S>;
  static constexpr std::size_t storage_dim = 2;
  static constexpr std::size_t tangent_dim = 1;
  static constexpr bool identity_tangent = false;

  T re{1}, im{0};

  constexpr Rot2() = default;
  constexpr Rot2(const T& r, const T& i) : re(r), im(i) {}

  static constexpr Rot2 identity() { return {T(1), T(0)}; }
  static constexpr Rot2 from_angle(const T& theta) { return {cos(theta), sin(theta)}; }
  static constexpr Rot2 from_tangent(const Vector<T, 1>& v, const T& /*epsilon*/ = T(0)) {
    return from_angle(v[0]);
  }
  constexpr Vector<T, 1> to_tangent(const T& epsilon = T(0)) const {
    Vector<T, 1> v;
    v[0] = atan2(im, re + copysign_no_zero(epsilon, re));
    return v;
  }
  constexpr T angle(const T& epsilon = T(0)) const { return to_tangent(epsilon)[0]; }

  constexpr Rot2 operator*(const Rot2& b) const { return {re * b.re - im * b.im, im * b.re + re * b.im}; }
  constexpr Vector2<T> operator*(const Vector2<T>& p) const {
    return Vector2<T>{re * p[0] - im * p[1], im * p[0] + re * p[1]};
  }
  constexpr Rot2 compose(const Rot2& b) const { return *this * b; }
  constexpr Rot2 inverse() const { return {re, -im}; }
  constexpr Rot2 between(const Rot2& b) const { return inverse() * b; }
  constexpr Matrix22<T> to_rotation_matrix() const { return Matrix22<T>{re, -im, im, re}; }

  constexpr Rot2 retract(const Vector<T, 1>& d, const T& epsilon = T(0)) const {
    return *this * from_tangent(d, epsilon);
  }
  constexpr Vector<T, 1> local_coordinates(const Rot2& b, const T& epsilon = T(0)) const {
    return between(b).to_tangent(epsilon);
  }
  // d(retract(this, δ))/dδ at δ = 0.
  constexpr Matrix<T, 2, 1> storage_D_tangent() const { return Matrix<T, 2, 1>{-im, re}; }
  // d(local_coordinates(this, y))/d(storage of y) at y = this (unit complex number).
  constexpr Matrix<T, 1, 2> tangent_D_storage() const { return Matrix<T, 1, 2>{-im, re}; }

  constexpr void to_storage(T* out) const {
    out[0] = re;
    out[1] = im;
  }
  static constexpr Rot2 from_storage(const T* in) { return {in[0], in[1]}; }
};

}  // namespace csym
