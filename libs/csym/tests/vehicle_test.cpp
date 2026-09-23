// Vehicle models beyond the SymForce oracle: RK4 factor (checked against finite differences) and
// compile-time evaluation.

#include <cstdio>

#include "vehicle_models.h"
#include "check.h"
#include "fd.h"

using namespace csym;
using V2 = Vector2<double>;
using V3 = Vector3<double>;
using V4 = Vector<double, 4>;
using V6 = Vector<double, 6>;
using P2 = Pose2<double>;

using TireFy = Function<vehicle::tire_fy, double, double, double, double, double, double, double, double, double,
                        double, double>;

// Compile-time evaluation of value and gradient.
constexpr auto kTire = TireFy::jacobian<0, 1, 2, 3>(25.0, 0.8, 0.3, 0.05, 1.2, 10.0, 1.4, 1.05, 4200.0, 0.2, 1e-9);
static_assert(kTire.value > 200.0 && kTire.value < 250.0);  // restoring force for this slip

int main() {
  // constexpr (csym::cm math) vs runtime (<cmath>) evaluation agree to a few ulp.
  const auto rt = TireFy::jacobian<0, 1, 2, 3>(25.0, 0.8, 0.3, 0.05, 1.2, 10.0, 1.4, 1.05, 4200.0, 0.2, 1e-9);
  CHECK_CLOSE(kTire.value, rt.value, 1e-13, 0.0);
  for (std::size_t i = 0; i < 4; ++i) CHECK_CLOSE(kTire.jacobian.data[i], rt.jacobian.data[i], 1e-12, 1e-12);

  // RK4 factor: value vs direct double evaluation, Jacobian vs central differences.
  using Rk4 = Function<vehicle::bicycle_factor_rk4, P2, V3, P2, V3, V2, V4, V4, V4, double, V6, double>;
  using Ref = fd::Reference<vehicle::bicycle_factor_rk4, P2, V3, P2, V3, V2, V4, V4, V4, double, V6, double>;
  const P2 pose_k = P2::from_angle_position(0.1, 10.0, 5.0), pose_k1 = P2::from_angle_position(0.12, 10.5, 5.1);
  const V3 vel_k{25.0, 0.5, 0.2}, vel_k1{25.2, 0.45, 0.22};
  const V2 u{0.04, 800.0};
  const V4 chassis{750.0, 1100.0, 1.3, 1.5}, tire_f{10.0, 1.4, 7000.0, 0.1}, tire_r{11.0, 1.4, 7500.0, 0.1};
  const V6 w{10.0, 10.0, 20.0, 5.0, 5.0, 8.0};
  const double dt = 0.02, eps = 1e-12;
  const auto [value, J] = Rk4::jacobian<0, 1, 2, 3>(pose_k, vel_k, pose_k1, vel_k1, u, chassis, tire_f, tire_r, dt,
                                                    w, eps);
  const auto ref = Ref::flat({pose_k, vel_k, pose_k1, vel_k1, u, chassis, tire_f, tire_r, dt, w, eps});
  for (std::size_t i = 0; i < 6; ++i) CHECK_CLOSE(value[i], ref[i], 1e-12, 1e-12);
  const auto Jfd = Ref::jacobian<4>(pose_k, vel_k, pose_k1, vel_k1, u, chassis, tire_f, tire_r, dt, w, eps);
  CHECK(Jfd.size() == J.data.size());
  for (std::size_t i = 0; i < J.data.size(); ++i) CHECK_CLOSE(J.data[i], Jfd[i], 1e-6, 1e-6);
  std::printf("bicycle rk4: ops value %zu, jacobian %zu, linearization %zu\n", Rk4::op_count<Mode::Value>,
              Rk4::op_count<Mode::Jacobian, Wrt<0, 1, 2, 3>>, Rk4::op_count<Mode::Linearization, Wrt<0, 1, 2, 3>>);
  return check::report("vehicle");
}
