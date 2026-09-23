#pragma once

// Numeric reference helpers: run a generic function directly on double types (no tape) and compute
// tangent-space Jacobians by central finite differences through retract().

#include <array>
#include <tuple>
#include <vector>

#include "csym/csym.h"

namespace fd {

// Perturbing a pose's translation retracts its rotation by exactly zero, which needs epsilon > 0.
inline constexpr double kRetractEpsilon = 1e-14;

template <auto F, class... Args>
struct Reference {
  using Tuple = std::tuple<Args...>;

  static std::vector<double> flat(const Tuple& args) {
    const auto o = std::apply([](const auto&... a) { return F(a...); }, args);
    using O = std::remove_cvref_t<decltype(o)>;
    std::array<double, csym::storage_dim<O>> buf{};
    csym::storage<O>::to(o, buf.data());
    return {buf.begin(), buf.end()};
  }

  // Column-major (outputs x total tangent dim of the first NW arguments).
  template <std::size_t NW = sizeof...(Args)>
  static std::vector<double> jacobian(const Args&... a, double h = 1e-6) {
    const Tuple base{a...};
    std::vector<double> J;
    [&]<std::size_t... I>(std::index_sequence<I...>) { (columns<I>(base, J, h), ...); }(
        std::make_index_sequence<NW>{});
    return J;
  }

  template <std::size_t I>
  static void columns(const Tuple& base, std::vector<double>& J, double h) {
    using A = std::tuple_element_t<I, Tuple>;
    constexpr std::size_t td = csym::tangent_dim<A>;
    for (std::size_t k = 0; k < td; ++k) {
      auto d = csym::Vector<double, td>::zero();
      d[k] = h;
      Tuple plus = base, minus = base;
      std::get<I>(plus) = csym::retract(std::get<I>(base), d, kRetractEpsilon);
      std::get<I>(minus) = csym::retract(std::get<I>(base), -d, kRetractEpsilon);
      const auto fp = flat(plus), fm = flat(minus);
      for (std::size_t r = 0; r < fp.size(); ++r) J.push_back((fp[r] - fm[r]) / (2 * h));
    }
  }
};

}  // namespace fd
