// Op counts of csym-generated code for functions also generated with SymForce (tools/symforce_oracle).
#include <cstdio>

#include "csym/csym.h"

using namespace csym;
using M = Mode;

constexpr auto rot3_act = [](auto R, auto p) { return R * p; };
constexpr auto rot3_compose = [](auto a, auto b) { return (a * b).to_rotation_matrix(); };
constexpr auto rot3_local = [](auto a, auto b, auto eps) { return local_coordinates(a, b, eps); };
constexpr auto pose3_between = [](auto a, auto b, auto a_T_b, auto sqrt_info, auto eps) {
  return between_residual(a, b, a_T_b, sqrt_info, eps);
};
constexpr auto pose2_between = [](auto a, auto b, auto a_T_b, auto sqrt_info, auto eps) {
  return between_residual(a, b, a_T_b, sqrt_info, eps);
};

template <class Fn, class W>
void row(const char* name, int sf_value, int sf_jac, int sf_lin) {
  std::printf("%-16s value %5zu (sf %5d)  jacobian %5zu (sf %5d)  linearization %5zu (sf %5d)\n", name,
              Fn::template op_count<M::Value>, sf_value, Fn::template op_count<M::Jacobian, W>, sf_jac,
              Fn::template op_count<M::Linearization, W>, sf_lin);
}

int main() {
  using R3 = Rot3<double>;
  row<Function<rot3_act, R3, Vector3<double>>, Wrt<0, 1>>("rot3_act", 43, 101, -1);
  row<Function<rot3_compose, R3, R3>, Wrt<0, 1>>("rot3_compose", 56, 343, -1);
  row<Function<optimized<rot3_compose>, R3, R3>, Wrt<0, 1>>("  (optimized)", 56, 343, -1);
  row<Function<rot3_local, R3, R3, double>, Wrt<0, 1>>("rot3_local", 42, 199, -1);
  row<Function<pose3_between, Pose3<double>, Pose3<double>, Pose3<double>, Matrix<double, 6, 6>, double>,
      Wrt<0, 1>>("pose3_between", 202, 1388, 2386);
  row<Function<pose2_between, Pose2<double>, Pose2<double>, Pose2<double>, Matrix33<double>, double>,
      Wrt<0, 1>>("pose2_between", 46, 120, 264);
}
