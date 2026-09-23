#pragma once

// Standard factor residuals, written generically over any Lie group value type. Use them inside a
// function passed to csym::Function to get compiled residuals and tangent-space Jacobians.
//
//   prior:   r = sqrt_info * local_coordinates(prior, x)
//   between: r = sqrt_info * local_coordinates(a_T_b, between(a, b))

#include "csym/lie.h"
#include "csym/matrix.h"

namespace csym {

template <class G, class T = scalar_of_t<G>, std::size_t N = lie<G>::tangent_dim>
constexpr Vector<T, N> prior_residual(const G& x, const G& prior, const Matrix<T, N, N>& sqrt_info,
                                      const T& epsilon) {
  return sqrt_info * local_coordinates(prior, x, epsilon);
}

template <class G, class T = scalar_of_t<G>, std::size_t N = lie<G>::tangent_dim>
constexpr Vector<T, N> between_residual(const G& a, const G& b, const G& a_T_b,
                                        const Matrix<T, N, N>& sqrt_info, const T& epsilon) {
  return sqrt_info * local_coordinates(a_T_b, between(a, b), epsilon);
}

}  // namespace csym
