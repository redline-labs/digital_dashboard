#include <Eigen/Dense>

#include "check.h"
#include "csym/adapters/eigen.h"
#include "csym/csym.h"

using namespace csym;

constexpr auto f = [](auto R, auto p) { return R * p; };
using Fn = Function<f, Rot3<double>, Vector3<double>>;

int main() {
  const Eigen::Quaterniond q = Eigen::Quaterniond(Eigen::AngleAxisd(0.7, Eigen::Vector3d(1, 2, 3).normalized()));
  const Eigen::Vector3d p(0.3, -1.0, 2.0);
  const auto [value, J] = Fn::jacobian(from_eigen(q), from_eigen(p));
  const Eigen::Vector3d expected = q * p;
  for (int i = 0; i < 3; ++i) CHECK_NEAR(to_eigen(value)(i), expected(i), 1e-12);
  // d(R p)/d(p) = R, and d(R p)/d(ω) = -R [p]x for right perturbations.
  const auto Jmap = eigen_map(J);
  const Eigen::Matrix3d R = q.toRotationMatrix();
  Eigen::Matrix3d px;
  px << 0, -p.z(), p.y(), p.z(), 0, -p.x(), -p.y(), p.x(), 0;
  const Eigen::Matrix3d dR = -R * px;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      CHECK_NEAR(Jmap(r, c), dR(r, c), 1e-12);
      CHECK_NEAR(Jmap(r, 3 + c), R(r, c), 1e-12);
    }
  return check::report("eigen");
}
