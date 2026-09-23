#pragma once

// Optional Eigen interop. csym itself has no dependencies; include this header only if you use Eigen.
//
//   Eigen::Vector3d v = csym::to_eigen(csym_vector);
//   csym::Matrix<double, 3, 1> m = csym::from_eigen(eigen_vector);
//   auto view = csym::eigen_map(lin.jacobian);   // zero-copy Eigen::Map over csym storage
//
// csym::Matrix is column-major like Eigen's default, so maps are zero-copy.

#include <Eigen/Core>

#include "csym/geo/pose2.h"
#include "csym/geo/pose3.h"
#include "csym/geo/rot2.h"
#include "csym/geo/rot3.h"
#include "csym/matrix.h"

namespace csym {

template <class T, std::size_t R, std::size_t C>
Eigen::Map<Eigen::Matrix<T, int(R), int(C)>> eigen_map(Matrix<T, R, C>& m) {
  return Eigen::Map<Eigen::Matrix<T, int(R), int(C)>>(m.data.data());
}
template <class T, std::size_t R, std::size_t C>
Eigen::Map<const Eigen::Matrix<T, int(R), int(C)>> eigen_map(const Matrix<T, R, C>& m) {
  return Eigen::Map<const Eigen::Matrix<T, int(R), int(C)>>(m.data.data());
}

template <class T, std::size_t R, std::size_t C>
Eigen::Matrix<T, int(R), int(C)> to_eigen(const Matrix<T, R, C>& m) {
  return eigen_map(m);
}

template <class Derived>
auto from_eigen(const Eigen::MatrixBase<Derived>& e) {
  using T = typename Derived::Scalar;
  constexpr int R = Derived::RowsAtCompileTime, C = Derived::ColsAtCompileTime;
  static_assert(R > 0 && C > 0, "from_eigen needs fixed-size matrices");
  Matrix<T, std::size_t(R), std::size_t(C)> m;
  eigen_map(m) = e;
  return m;
}

// Rotations: csym stores quaternions as [x, y, z, w], like Eigen's coeffs().
template <class T>
Eigen::Quaternion<T> to_eigen(const Rot3<T>& r) {
  return Eigen::Quaternion<T>(r.w, r.x, r.y, r.z);
}
template <class T>
Rot3<T> from_eigen(const Eigen::Quaternion<T>& q) {
  return Rot3<T>(q.x(), q.y(), q.z(), q.w());
}

}  // namespace csym
