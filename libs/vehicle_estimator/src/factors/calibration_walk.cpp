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
using V9 = csym::Vector<double, 9>;
using Rot = csym::Rot3<double>;
constexpr std::size_t kN = static_cast<std::size_t>(kCalibrationDim);

// One segment of the installation to the next: each part may have wandered
// by its own density times sqrt(dt), independently. `w` is the per-dimension
// whitening, 1 / (q sqrt(dt)), in tangent order.
constexpr auto kCalibrationWalk = [](auto m0, auto l0, auto s0, auto k0, auto o0, auto m1, auto l1, auto s1, auto k1,
                                     auto o1, auto w, auto eps) {
    using T = decltype(eps);
    const auto dm = csym::local_coordinates(m0, m1, eps);
    const auto dl = l1 - l0;
    const auto ds = s1 - s0;
    const auto dk = k1 - k0;
    const auto dob = o1 - o0;
    return csym::Vector<T, kN>{dm[0] * w[0],   dm[1] * w[1],   dm[2] * w[2],   dl[0] * w[3],   dl[1] * w[4],
                               dl[2] * w[5],   ds[0] * w[6],   ds[1] * w[7],   dk[0] * w[8],   dk[1] * w[9],
                               dk[2] * w[10],  dk[3] * w[11],  dk[4] * w[12],  dk[5] * w[13],  dk[6] * w[14],
                               dk[7] * w[15],  dk[8] * w[16],  dob[0] * w[17], dob[1] * w[18]};
};

using CalibrationWalkFactor =
    factor_graph::CsymFactor<kCalibrationWalk, factor_graph::Vars<Rot, V3, V2, V9, V2, Rot, V3, V2, V9, V2>,
                             factor_graph::Params<csym::Vector<double, kN>, double>>;

}  // namespace

FactorPtr calibrationWalk(const CalibrationKeys& from, const CalibrationKeys& to, double dt,
                          const Eigen::Matrix<double, kCalibrationDim, 1>& densities)
{
    if (!(dt > 0.0) || !(densities.minCoeff() > 0.0))
        throw std::invalid_argument("calibration walk: dt and every density must be positive");
    csym::Vector<double, kN> w;
    for (std::size_t i = 0; i < kN; ++i) w[i] = 1.0 / (densities[static_cast<Eigen::Index>(i)] * std::sqrt(dt));
    return std::make_shared<CalibrationWalkFactor>(
        "calibration walk",
        std::array<Key, 10>{from.mounting, from.lever_arm, from.boresight, from.magnetometer, from.barometer,
                            to.mounting, to.lever_arm, to.boresight, to.magnetometer, to.barometer},
        w, factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
