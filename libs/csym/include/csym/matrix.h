#pragma once

// Fixed-size, column-major matrix over any scalar (double, float or Expr). Storage order matches
// SymForce's matrix storage (column-major) and Eigen's default layout.

#include <array>
#include <cstddef>

#include "csym/core/storage.h"

namespace csym {

template <class T, std::size_t R, std::size_t C>
class Matrix {
 public:
  using Scalar = T;
  template <class S>
  using rebind = Matrix<S, R, C>;
  static constexpr std::size_t rows = R, cols = C, size = R * C;
  static constexpr std::size_t storage_dim = R * C;

  std::array<T, R * C> data{};

  constexpr Matrix() = default;
  // Row-major initializer list for readability: Matrix<T,2,2>{a, b, c, d} is [[a, b], [c, d]].
  template <class... A>
    requires(sizeof...(A) == R * C && sizeof...(A) > 1)
  constexpr Matrix(const A&... a) {  // NOLINT
    const T vals[] = {T(a)...};
    for (std::size_t r = 0; r < R; ++r)
      for (std::size_t c = 0; c < C; ++c) (*this)(r, c) = vals[r * C + c];
  }

  static constexpr Matrix zero() {
    Matrix m;
    for (auto& x : m.data) x = T(0);
    return m;
  }
  static constexpr Matrix identity() {
    Matrix m = zero();
    for (std::size_t i = 0; i < (R < C ? R : C); ++i) m(i, i) = T(1);
    return m;
  }

  constexpr T& operator()(std::size_t r, std::size_t c) { return data[c * R + r]; }
  constexpr const T& operator()(std::size_t r, std::size_t c) const { return data[c * R + r]; }
  constexpr T& operator[](std::size_t i) { return data[i]; }
  constexpr const T& operator[](std::size_t i) const { return data[i]; }

  constexpr T x() const requires(C == 1 && R >= 1) { return data[0]; }
  constexpr T y() const requires(C == 1 && R >= 2) { return data[1]; }
  constexpr T z() const requires(C == 1 && R >= 3) { return data[2]; }

  // ---- storage --------------------------------------------------------------------------------
  constexpr void to_storage(T* out) const {
    for (std::size_t i = 0; i < R * C; ++i) out[i] = data[i];
  }
  static constexpr Matrix from_storage(const T* in) {
    Matrix m;
    for (std::size_t i = 0; i < R * C; ++i) m.data[i] = in[i];
    return m;
  }

  // ---- arithmetic -----------------------------------------------------------------------------
  friend constexpr Matrix operator+(const Matrix& a, const Matrix& b) {
    Matrix m;
    for (std::size_t i = 0; i < R * C; ++i) m.data[i] = a.data[i] + b.data[i];
    return m;
  }
  friend constexpr Matrix operator-(const Matrix& a, const Matrix& b) {
    Matrix m;
    for (std::size_t i = 0; i < R * C; ++i) m.data[i] = a.data[i] - b.data[i];
    return m;
  }
  friend constexpr Matrix operator-(const Matrix& a) {
    Matrix m;
    for (std::size_t i = 0; i < R * C; ++i) m.data[i] = -a.data[i];
    return m;
  }
  friend constexpr Matrix operator*(const Matrix& a, const T& s) {
    Matrix m;
    for (std::size_t i = 0; i < R * C; ++i) m.data[i] = a.data[i] * s;
    return m;
  }
  friend constexpr Matrix operator*(const T& s, const Matrix& a) { return a * s; }
  friend constexpr Matrix operator/(const Matrix& a, const T& s) {
    Matrix m;
    for (std::size_t i = 0; i < R * C; ++i) m.data[i] = a.data[i] / s;
    return m;
  }
  constexpr Matrix& operator+=(const Matrix& b) { return *this = *this + b; }
  constexpr Matrix& operator-=(const Matrix& b) { return *this = *this - b; }

  template <std::size_t K>
  friend constexpr Matrix<T, R, K> operator*(const Matrix& a, const Matrix<T, C, K>& b) {
    Matrix<T, R, K> m;
    for (std::size_t r = 0; r < R; ++r)
      for (std::size_t k = 0; k < K; ++k) {
        T acc = a(r, 0) * b(0, k);
        for (std::size_t c = 1; c < C; ++c) acc = acc + a(r, c) * b(c, k);
        m(r, k) = acc;
      }
    return m;
  }

  constexpr Matrix<T, C, R> transpose() const {
    Matrix<T, C, R> m;
    for (std::size_t r = 0; r < R; ++r)
      for (std::size_t c = 0; c < C; ++c) m(c, r) = (*this)(r, c);
    return m;
  }

  template <std::size_t BR, std::size_t BC>
  constexpr Matrix<T, BR, BC> block(std::size_t r0, std::size_t c0) const {
    Matrix<T, BR, BC> m;
    for (std::size_t r = 0; r < BR; ++r)
      for (std::size_t c = 0; c < BC; ++c) m(r, c) = (*this)(r0 + r, c0 + c);
    return m;
  }
  template <std::size_t BR, std::size_t BC>
  constexpr void set_block(std::size_t r0, std::size_t c0, const Matrix<T, BR, BC>& b) {
    for (std::size_t r = 0; r < BR; ++r)
      for (std::size_t c = 0; c < BC; ++c) (*this)(r0 + r, c0 + c) = b(r, c);
  }

  constexpr T squared_norm() const {
    T acc = data[0] * data[0];
    for (std::size_t i = 1; i < R * C; ++i) acc = acc + data[i] * data[i];
    return acc;
  }
  // sqrt(|x|^2 + epsilon): finite derivative at zero when epsilon > 0. (Note: epsilon, not epsilon^2,
  // matching SymForce's convention for norms.)
  constexpr T norm(const T& epsilon = T(0)) const { return sqrt(squared_norm() + epsilon); }
  constexpr Matrix normalized(const T& epsilon = T(0)) const { return *this / norm(epsilon); }

  constexpr T dot(const Matrix& b) const requires(C == 1) {
    T acc = data[0] * b.data[0];
    for (std::size_t i = 1; i < R; ++i) acc = acc + data[i] * b.data[i];
    return acc;
  }
  constexpr Matrix cross(const Matrix& b) const requires(R == 3 && C == 1) {
    const auto& a = *this;
    return Matrix{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  }
  // Skew-symmetric matrix [v]x such that [v]x * w == v.cross(w).
  constexpr Matrix<T, 3, 3> skew() const requires(R == 3 && C == 1) {
    const auto& v = *this;
    return Matrix<T, 3, 3>{T(0), -v[2], v[1], v[2], T(0), -v[0], -v[1], v[0], T(0)};
  }

  // ---- lie group (vector space) ---------------------------------------------------------------
  static constexpr std::size_t tangent_dim = R * C;
  static constexpr bool identity_tangent = true;
  constexpr Matrix compose(const Matrix& b) const { return *this + b; }
  constexpr Matrix inverse() const { return -*this; }
  constexpr Matrix retract(const Matrix<T, R * C, 1>& d, const T& /*epsilon*/ = T(0)) const {
    return *this + Matrix::from_storage(d.data.data());
  }
  constexpr Matrix<T, R * C, 1> local_coordinates(const Matrix& b, const T& /*epsilon*/ = T(0)) const {
    return Matrix<T, R * C, 1>::from_storage((b - *this).data.data());
  }
};

template <class T, std::size_t N>
using Vector = Matrix<T, N, 1>;
template <class T>
using Vector2 = Matrix<T, 2, 1>;
template <class T>
using Vector3 = Matrix<T, 3, 1>;
template <class T>
using Matrix22 = Matrix<T, 2, 2>;
template <class T>
using Matrix33 = Matrix<T, 3, 3>;

}  // namespace csym
