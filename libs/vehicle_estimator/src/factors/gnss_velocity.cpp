#include "vehicle_estimator/factors.h"

#include "common.h"

#include "csym/noise_models.h"
#include "factor_graph/csym_factor.h"
#include "factor_graph/values.h"

namespace vehicle_estimator::factors
{

namespace
{

using V3 = csym::Vector3<double>;
using M33 = csym::Matrix33<double>;

// The lever arm matters here in a way it barely does for position: at
// 1 rad/s of yaw a metre of lever arm is a metre per second at the antenna.
constexpr auto kGnssVelocity = [](auto R, auto v, auto la, auto measured, auto omega, auto sqrt_info, auto delta,
                                  auto eps) {
    using T = decltype(delta);
    const auto r = sqrt_info * (v + R * omega.cross(la) - measured);
    return csym::PseudoHuberNoiseModel<T>(delta, T(1), eps).whiten_norm(r, eps);
};

using GnssVelocityFactor = factor_graph::CsymFactor<kGnssVelocity, factor_graph::Vars<csym::Rot3<double>, V3, V3>,
                                                    factor_graph::Params<V3, V3, M33, double, double>>;

}  // namespace

FactorPtr gnssVelocity(Key R, Key v, Key la, const Eigen::Vector3d& measured_e, const Eigen::Matrix3d& cov_e,
                       const Eigen::Vector3d& omega_i, double robust_delta)
{
    return std::make_shared<GnssVelocityFactor>("gnss velocity", std::array<Key, 3>{R, v, la},
                                                detail::toCsym3(measured_e), detail::toCsym3(omega_i),
                                                detail::toCsym<3, 3>(sqrtInformation(cov_e)), robust_delta,
                                                factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
