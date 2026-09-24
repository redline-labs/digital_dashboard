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

// Parked: the IMU is not moving over the ground. Robust, because the gate
// that decides "parked" is judged a moment before the car rolls away, and a
// zero-velocity claim made as it does must not pull with unbounded force.
constexpr auto kZeroVelocity = [](auto v, auto inv_sigma, auto delta, auto eps) {
    using T = decltype(delta);
    return csym::PseudoHuberNoiseModel<T>(delta, T(1), eps).whiten_norm(v * inv_sigma, eps);
};

using ZeroVelocityFactor =
    factor_graph::CsymFactor<kZeroVelocity, factor_graph::Vars<V3>, factor_graph::Params<double, double, double>>;

}  // namespace

FactorPtr zeroVelocity(Key v, double sigma, double robust_delta)
{
    return std::make_shared<ZeroVelocityFactor>("zero velocity", std::array<Key, 1>{v}, 1.0 / sigma, robust_delta,
                                                factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
