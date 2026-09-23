#pragma once

// Shared by the geo_* tests, which are split one TU per group because a
// single TU holding all nine functions took 151 s to compile.

#include <cstdio>

#include "check.h"
#include "csym/csym.h"
#include "fd.h"

namespace geo_check {

using namespace csym;

// Compares a compiled Function against direct double evaluation and finite differences. The last
// argument is epsilon, which is excluded from the Jacobian check: finite differences straddle |epsilon|.
template <auto F, class... Args>
inline void check_function(const char* name, const Args&... args) {
  constexpr std::size_t NW = sizeof...(Args) - 1;
  using Fn = Function<F, Args...>;
  using Ref = fd::Reference<F, Args...>;
  const auto ref = Ref::flat(std::tuple<Args...>{args...});
  const auto [value, J] = [&]<std::size_t... I>(std::index_sequence<I...>) {
    return Fn::template jacobian<I...>(args...);
  }(std::make_index_sequence<NW>{});
  std::array<double, Fn::output_dim> got{};
  storage<typename Fn::Output>::to(value, got.data());
  for (std::size_t i = 0; i < got.size(); ++i) CHECK_CLOSE(got[i], ref[i], 1e-12, 1e-12);
  const auto Jfd = Ref::template jacobian<NW>(args...);
  CHECK(Jfd.size() == J.data.size());
  for (std::size_t i = 0; i < J.data.size(); ++i) CHECK_CLOSE(J.data[i], Jfd[i], 1e-6, 1e-7);
  // eval() must agree with the value from jacobian()
  std::array<double, Fn::output_dim> v2{};
  storage<typename Fn::Output>::to(Fn::eval(args...), v2.data());
  for (std::size_t i = 0; i < got.size(); ++i) CHECK_CLOSE(v2[i], got[i], 1e-14, 1e-14);
  std::printf("%-22s ops: value %4zu  jacobian %4zu  linearization %4zu\n", name,
              Fn::template op_count<Mode::Value>, Fn::template op_count<Mode::Jacobian>,
              Fn::template op_count<Mode::Linearization>);
}

}  // namespace geo_check
