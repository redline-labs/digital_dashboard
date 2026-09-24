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
using Rot = csym::Rot3<double>;

// The whole installation at once, so a prior carried over from a previous
// run keeps the correlations it learned -- the lever arm and the boresight
// are seen through the same antenna and are not independent.
constexpr auto kCalibrationPrior = [](auto m, auto l, auto s, auto m0, auto l0, auto s0, auto sqrt_info, auto eps) {
    using T = decltype(eps);
    const auto dm = csym::local_coordinates(m0, m, eps);
    const auto dl = l - l0;
    const auto ds = s - s0;
    return sqrt_info * csym::Vector<T, 8>{dm[0], dm[1], dm[2], dl[0], dl[1], dl[2], ds[0], ds[1]};
};

using CalibrationPriorFactor =
    factor_graph::CsymFactor<kCalibrationPrior, factor_graph::Vars<Rot, V3, V2>,
                             factor_graph::Params<Rot, V3, V2, csym::Matrix<double, 8, 8>, double>>;

}  // namespace

FactorPtr calibrationPrior(const CalibrationKeys& keys, const CalibrationSet& prior)
{
    return std::make_shared<CalibrationPriorFactor>(
        "calibration prior", std::array<Key, 3>{keys.mounting, keys.lever_arm, keys.boresight},
        detail::toCsymRot(prior.mounting), detail::toCsym3(prior.lever_arm),
        V2{prior.boresight.x(), prior.boresight.y()}, detail::toCsym<8, 8>(sqrtInformation(prior.cov)),
        factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
