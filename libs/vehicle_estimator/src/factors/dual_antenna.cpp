#include "vehicle_estimator/factors.h"

#include "common.h"

#include "csym/noise_models.h"
#include "factor_graph/csym_factor.h"
#include "factor_graph/values.h"

#include <cmath>
#include <stdexcept>

namespace vehicle_estimator::factors
{

namespace
{

using V2 = csym::Vector2<double>;
using V3 = csym::Vector3<double>;
using M22 = csym::Matrix22<double>;
using M33 = csym::Matrix33<double>;

// Two antennas give the direction of the line between them and nothing about
// rotation around it, so this is two residuals, not three: roll comes from
// the IMU. The yaw difference is wrapped through atan2(sin, cos), which csym
// can differentiate and which does not care which side of north it is on.
constexpr auto kDualAntenna = [](auto R, auto bs, auto R_n_e, auto b0, auto u1, auto u2, auto measured,
                                 auto sqrt_info, auto delta, auto eps) {
    using T = decltype(delta);
    const auto b_i = (b0 + u1 * bs[0] + u2 * bs[1]).normalized(eps);
    const auto b_n = R_n_e * (R * b_i);
    const T yaw = csym::atan2(b_n[1], b_n[0]);
    const T pitch = csym::atan2(-b_n[2], csym::sqrt(b_n[0] * b_n[0] + b_n[1] * b_n[1] + eps));
    const T dyaw = yaw - measured[0];
    csym::Vector2<T> e{csym::atan2(csym::sin(dyaw), csym::cos(dyaw)), pitch - measured[1]};
    return csym::PseudoHuberNoiseModel<T>(delta, T(1), eps).whiten_norm(sqrt_info * e, eps);
};

using DualAntennaFactor =
    factor_graph::CsymFactor<kDualAntenna, factor_graph::Vars<csym::Rot3<double>, V2>,
                             factor_graph::Params<M33, V3, V3, V3, V2, M22, double, double>>;

}  // namespace

Baseline Baseline::fromAntennas(const Eigen::Vector3d& antenna1_i, const Eigen::Vector3d& antenna2_i)
{
    const Eigen::Vector3d d = antenna2_i - antenna1_i;
    if (!(d.norm() > 0.05)) throw std::invalid_argument("antenna baseline shorter than 5 cm");
    Baseline b;
    b.b0 = d.normalized();
    // Any perpendicular pair: the boresight is a two-degree-of-freedom tilt
    // and has no preferred axes.
    const Eigen::Vector3d seed = std::fabs(b.b0.z()) < 0.9 ? Eigen::Vector3d::UnitZ() : Eigen::Vector3d::UnitX();
    b.u1 = b.b0.cross(seed).normalized();
    b.u2 = b.b0.cross(b.u1).normalized();
    return b;
}

Eigen::Vector3d Baseline::direction(const Eigen::Vector2d& bs) const
{
    return (b0 + bs.x() * u1 + bs.y() * u2).normalized();
}

FactorPtr dualAntenna(Key R, Key bs, const Eigen::Matrix3d& R_n_e, const Baseline& baseline, double yaw,
                      double pitch, const Eigen::Matrix2d& cov, double robust_delta)
{
    return std::make_shared<DualAntennaFactor>(
        "dual antenna", std::array<Key, 2>{R, bs}, detail::toCsym<3, 3>(R_n_e), detail::toCsym3(baseline.b0),
        detail::toCsym3(baseline.u1), detail::toCsym3(baseline.u2), V2{yaw, pitch},
        detail::toCsym<2, 2>(sqrtInformation(cov)), robust_delta, factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
