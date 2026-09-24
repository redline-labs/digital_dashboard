#include "vehicle_estimator/factors.h"

#include "common.h"

#include "factor_graph/csym_factor.h"
#include "factor_graph/values.h"

namespace vehicle_estimator::factors
{

namespace
{

using V2 = csym::Vector2<double>;
using V3 = csym::Vector3<double>;
using V9 = csym::Vector<double, 9>;
using Rot = csym::Rot3<double>;
constexpr std::size_t kN = static_cast<std::size_t>(kCalibrationDim);

// The whole installation at once, so a prior carried over from a previous
// run keeps the correlations it learned -- the lever arm and the boresight
// are seen through the same antenna and are not independent.
constexpr auto kCalibrationPrior = [](auto m, auto l, auto s, auto k, auto o, auto m0, auto l0, auto s0, auto k0,
                                      auto o0, auto sqrt_info, auto eps) {
    using T = decltype(eps);
    const auto dm = csym::local_coordinates(m0, m, eps);
    const auto dl = l - l0;
    const auto ds = s - s0;
    const auto dk = k - k0;
    const auto dob = o - o0;
    csym::Vector<T, kN> e{dm[0], dm[1], dm[2], dl[0], dl[1], dl[2], ds[0], ds[1], dk[0], dk[1],
                          dk[2], dk[3], dk[4], dk[5], dk[6], dk[7], dk[8], dob[0], dob[1]};
    return sqrt_info * e;
};

using CalibrationPriorFactor =
    factor_graph::CsymFactor<kCalibrationPrior, factor_graph::Vars<Rot, V3, V2, V9, V2>,
                             factor_graph::Params<Rot, V3, V2, V9, V2, csym::Matrix<double, kN, kN>, double>>;

}  // namespace

FactorPtr calibrationPrior(const CalibrationKeys& keys, const CalibrationSet& prior)
{
    V9 k0;
    const Vector9d k = prior.magnetometer();
    for (std::size_t i = 0; i < 9; ++i) k0[i] = k[static_cast<Eigen::Index>(i)];
    return std::make_shared<CalibrationPriorFactor>(
        "calibration prior",
        std::array<Key, 5>{keys.mounting, keys.lever_arm, keys.boresight, keys.magnetometer, keys.barometer},
        detail::toCsymRot(prior.mounting), detail::toCsym3(prior.lever_arm),
        V2{prior.boresight.x(), prior.boresight.y()}, k0, V2{prior.baro_offset, prior.baro_airflow},
        detail::toCsym<kN, kN>(sqrtInformation(prior.cov)), factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
