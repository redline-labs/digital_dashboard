#include "vehicle_estimator/factors.h"

#include "common.h"

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
using Rot = csym::Rot3<double>;

// One segment of the installation to the next: each part may have wandered
// by its density times sqrt(dt), independently.
constexpr auto kCalibrationWalk = [](auto m0, auto l0, auto s0, auto m1, auto l1, auto s1, auto w_m, auto w_l,
                                     auto w_s, auto eps) {
    using T = decltype(eps);
    const auto dm = csym::local_coordinates(m0, m1, eps) * w_m;
    const auto dl = (l1 - l0) * w_l;
    const auto ds = (s1 - s0) * w_s;
    return csym::Vector<T, 8>{dm[0], dm[1], dm[2], dl[0], dl[1], dl[2], ds[0], ds[1]};
};

using CalibrationWalkFactor =
    factor_graph::CsymFactor<kCalibrationWalk, factor_graph::Vars<Rot, V3, V2, Rot, V3, V2>,
                             factor_graph::Params<double, double, double, double>>;

}  // namespace

FactorPtr calibrationWalk(const CalibrationKeys& from, const CalibrationKeys& to, double dt, double mounting_walk,
                          double lever_arm_walk, double boresight_walk)
{
    if (!(dt > 0.0) || !(mounting_walk > 0.0) || !(lever_arm_walk > 0.0) || !(boresight_walk > 0.0))
        throw std::invalid_argument("calibration walk: dt and every density must be positive");
    const double root = std::sqrt(dt);
    return std::make_shared<CalibrationWalkFactor>(
        "calibration walk",
        std::array<Key, 6>{from.mounting, from.lever_arm, from.boresight, to.mounting, to.lever_arm, to.boresight},
        1.0 / (mounting_walk * root), 1.0 / (lever_arm_walk * root), 1.0 / (boresight_walk * root),
        factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
