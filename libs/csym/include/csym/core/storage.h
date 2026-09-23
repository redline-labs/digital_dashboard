#pragma once

// Storage and rebind traits: how a value type flattens to a list of scalars, and how the same type is
// expressed over a different scalar (double <-> Expr).
//
// Class types opt in with members:
//   using Scalar = T;
//   template <class S> using rebind = X<S>;
//   static constexpr std::size_t storage_dim;
//   constexpr void to_storage(T* out) const;
//   static constexpr X from_storage(const T* in);

#include <array>
#include <cstddef>
#include <tuple>
#include <type_traits>

#include "csym/core/expr.h"

namespace csym {

// ---- rebind ------------------------------------------------------------------------------------
template <class T, class S>
struct rebind {
  using type = typename T::template rebind<S>;
};
template <Scalar T, class S>
struct rebind<T, S> {
  using type = S;
};
template <class... Ts, class S>
struct rebind<std::tuple<Ts...>, S> {
  using type = std::tuple<typename rebind<Ts, S>::type...>;
};
template <class T, std::size_t N, class S>
struct rebind<std::array<T, N>, S> {
  using type = std::array<typename rebind<T, S>::type, N>;
};
template <class T, class S>
using rebind_t = typename rebind<std::remove_cvref_t<T>, S>::type;

// ---- scalar type -------------------------------------------------------------------------------
template <class T>
struct scalar_of {
  using type = typename T::Scalar;
};
template <Scalar T>
struct scalar_of<T> {
  using type = T;
};
template <class T, class... Ts>
struct scalar_of<std::tuple<T, Ts...>> {
  using type = typename scalar_of<T>::type;
};
template <class T, std::size_t N>
struct scalar_of<std::array<T, N>> {
  using type = typename scalar_of<T>::type;
};
template <class T>
using scalar_of_t = typename scalar_of<std::remove_cvref_t<T>>::type;

// ---- storage -----------------------------------------------------------------------------------
template <class T>
struct storage {
  static constexpr std::size_t dim = T::storage_dim;
  template <class S>
  static constexpr void to(const T& x, S* out) { x.to_storage(out); }
  template <class S>
  static constexpr T from(const S* in) { return T::from_storage(in); }
};
template <Scalar T>
struct storage<T> {
  static constexpr std::size_t dim = 1;
  static constexpr void to(const T& x, T* out) { out[0] = x; }
  static constexpr T from(const T* in) { return in[0]; }
};
template <class... Ts>
struct storage<std::tuple<Ts...>> {
  static constexpr std::size_t dim = (storage<Ts>::dim + ... + 0);
  template <class S>
  static constexpr void to(const std::tuple<Ts...>& x, S* out) {
    std::apply([&](const auto&... e) { ((storage<std::remove_cvref_t<decltype(e)>>::to(e, out),
                                         out += storage<std::remove_cvref_t<decltype(e)>>::dim),
                                        ...); },
               x);
  }
  template <class S>
  static constexpr std::tuple<Ts...> from(const S* in) {
    constexpr std::array<std::size_t, sizeof...(Ts) + 1> off = [] {
      std::array<std::size_t, sizeof...(Ts) + 1> o{};
      std::size_t k = 0, acc = 0;
      ((o[k++] = acc, acc += storage<Ts>::dim), ...);
      o[k] = acc;
      return o;
    }();
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      return std::tuple<Ts...>{storage<Ts>::from(in + off[I])...};
    }(std::index_sequence_for<Ts...>{});
  }
};
template <class T, std::size_t N>
struct storage<std::array<T, N>> {
  static constexpr std::size_t dim = N * storage<T>::dim;
  template <class S>
  static constexpr void to(const std::array<T, N>& x, S* out) {
    for (std::size_t i = 0; i < N; ++i) storage<T>::to(x[i], out + i * storage<T>::dim);
  }
  template <class S>
  static constexpr std::array<T, N> from(const S* in) {
    std::array<T, N> x{};
    for (std::size_t i = 0; i < N; ++i) x[i] = storage<T>::from(in + i * storage<T>::dim);
    return x;
  }
};

template <class T>
inline constexpr std::size_t storage_dim = storage<std::remove_cvref_t<T>>::dim;

}  // namespace csym
