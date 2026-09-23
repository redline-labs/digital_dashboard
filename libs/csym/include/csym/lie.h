#pragma once

// Lie group traits: how a value type moves on its manifold, and how its storage changes with respect to
// its tangent space.
//
// Class types opt in with members:
//   static constexpr std::size_t tangent_dim;
//   static constexpr bool identity_tangent;   // true when tangent == storage (vector spaces)
//   X retract(const Vector<T, tangent_dim>& delta, const T& epsilon) const;
//   Vector<T, tangent_dim> local_coordinates(const X& b, const T& epsilon) const;
//   Matrix<T, storage_dim, tangent_dim> storage_D_tangent() const;   // unless identity_tangent
//   Matrix<T, tangent_dim, storage_dim> tangent_D_storage() const;   // unless identity_tangent
//
// Jacobians produced by csym are taken with respect to the tangent space, using the chain rule
// d(out)/d(tangent) = d(out)/d(storage) * storage_D_tangent, where storage_D_tangent is the derivative
// of retract(x, delta) with respect to delta at delta = 0. When the *output* is a Lie group, its
// Jacobian rows are in its tangent space too (like SymForce): tangent_D_storage is the derivative of
// local_coordinates(y0, y) with respect to y's storage at y = y0.

#include <cstddef>

#include "csym/matrix.h"

namespace csym {

template <class T>
struct lie {
  static constexpr std::size_t tangent_dim = T::tangent_dim;
  static constexpr bool identity_tangent = T::identity_tangent;
  using S = typename T::Scalar;
  static constexpr T retract(const T& a, const Vector<S, tangent_dim>& d, const S& eps) {
    return a.retract(d, eps);
  }
  static constexpr Vector<S, tangent_dim> local_coordinates(const T& a, const T& b, const S& eps) {
    return a.local_coordinates(b, eps);
  }
  static constexpr auto storage_D_tangent(const T& a) { return a.storage_D_tangent(); }
  static constexpr auto tangent_D_storage(const T& a) { return a.tangent_D_storage(); }
};

template <Scalar T>
struct lie<T> {
  static constexpr std::size_t tangent_dim = 1;
  static constexpr bool identity_tangent = true;
  static constexpr T retract(const T& a, const Vector<T, 1>& d, const T&) { return a + d[0]; }
  static constexpr Vector<T, 1> local_coordinates(const T& a, const T& b, const T&) {
    Vector<T, 1> v;
    v[0] = b - a;
    return v;
  }
};

// Group operations usable for any value type (scalars and matrices are additive groups).
template <class G>
constexpr G between(const G& a, const G& b) {
  if constexpr (lie<G>::identity_tangent) return b - a;
  else return a.between(b);
}
template <class G>
constexpr auto local_coordinates(const G& a, const G& b, const scalar_of_t<G>& epsilon = scalar_of_t<G>(0)) {
  return lie<G>::local_coordinates(a, b, epsilon);
}
template <class G>
constexpr G retract(const G& a, const Vector<scalar_of_t<G>, lie<G>::tangent_dim>& d,
                    const scalar_of_t<G>& epsilon = scalar_of_t<G>(0)) {
  return lie<G>::retract(a, d, epsilon);
}

template <class T>
inline constexpr std::size_t tangent_dim = lie<std::remove_cvref_t<T>>::tangent_dim;

}  // namespace csym
