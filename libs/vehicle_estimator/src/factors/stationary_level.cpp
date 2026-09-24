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
using Rot = csym::Rot3<double>;

// A parked car's body is level, to within the grade and camber of wherever it
// stopped. The IMU knows its own tilt from gravity, so what this says is how
// the IMU sits in the body: the mounting's roll and pitch. One stop is worth
// little; many, facing many ways, average the road out.
constexpr auto kStationaryLevel = [](auto R, auto m, auto R_n_e, auto inv_sigma, auto delta, auto eps) {
    using T = decltype(delta);
    const auto R_n_b = R_n_e * (R * m.inverse()).to_rotation_matrix();
    const T roll = csym::atan2(R_n_b(2, 1), R_n_b(2, 2));
    const T pitch = csym::atan2(-R_n_b(2, 0), csym::sqrt(R_n_b(2, 1) * R_n_b(2, 1) + R_n_b(2, 2) * R_n_b(2, 2) + eps));
    csym::Vector2<T> e{inv_sigma[0] * roll, inv_sigma[1] * pitch};
    return csym::PseudoHuberNoiseModel<T>(delta, T(1), eps).whiten_norm(e, eps);
};

using StationaryLevelFactor =
    factor_graph::CsymFactor<kStationaryLevel, factor_graph::Vars<Rot, Rot>,
                             factor_graph::Params<csym::Matrix33<double>, V2, double, double>>;

}  // namespace

FactorPtr stationaryLevel(Key R, Key mounting, const Eigen::Matrix3d& R_n_e, double sigma, double robust_delta)
{
    return std::make_shared<StationaryLevelFactor>("stationary level", std::array<Key, 2>{R, mounting},
                                                   detail::toCsym<3, 3>(R_n_e), V2{1.0 / sigma, 1.0 / sigma},
                                                   robust_delta, factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
