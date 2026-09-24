#include "vehicle_estimator/factors.h"

#include "common.h"

#include "csym/noise_models.h"
#include "factor_graph/csym_factor.h"
#include "factor_graph/values.h"

namespace vehicle_estimator::factors
{

namespace
{

using V2 = csym::Vector2<double>;
using V3 = csym::Vector3<double>;
using Rot = csym::Rot3<double>;

// A car running straight and true moves along its own x axis: no sideslip,
// no heave. Said of the body, through the mounting, it pins the mounting's
// yaw (sideslip) and pitch (heave) -- the IMU's own attitude is already known
// from gravity and the antennas, so the mounting is what gives. Angles, not
// velocities, so the sigma means the same at any speed.
constexpr auto kStraightDriving = [](auto R, auto v, auto m, auto omega, auto r_ref, auto inv_sigma, auto delta,
                                     auto eps) {
    using T = decltype(delta);
    const auto v_i = R.inverse() * v + omega.cross(r_ref);
    const auto v_b = m * v_i;
    const T speed = csym::sqrt(v_b[0] * v_b[0] + v_b[1] * v_b[1] + v_b[2] * v_b[2] + eps);
    csym::Vector2<T> e{inv_sigma[0] * v_b[1] / speed, inv_sigma[1] * v_b[2] / speed};
    return csym::PseudoHuberNoiseModel<T>(delta, T(1), eps).whiten_norm(e, eps);
};

using StraightDrivingFactor =
    factor_graph::CsymFactor<kStraightDriving, factor_graph::Vars<Rot, V3, Rot>,
                             factor_graph::Params<V3, V3, V2, double, double>>;

}  // namespace

FactorPtr straightDriving(Key R, Key v, Key mounting, const Eigen::Vector3d& omega_i, const Eigen::Vector3d& r_ref,
                          double sigma_slip, double sigma_heave, double robust_delta)
{
    return std::make_shared<StraightDrivingFactor>("straight driving", std::array<Key, 3>{R, v, mounting},
                                                   detail::toCsym3(omega_i), detail::toCsym3(r_ref),
                                                   V2{1.0 / sigma_slip, 1.0 / sigma_heave}, robust_delta,
                                                   factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
