#include "vehicle_estimator/factors.h"

#include "common.h"

#include "vehicle_estimator/atmosphere.h"

#include "csym/noise_models.h"
#include "factor_graph/csym_factor.h"
#include "factor_graph/values.h"

namespace vehicle_estimator::factors
{

namespace
{

using V2 = csym::Vector2<double>;
using V3 = csym::Vector3<double>;

// The barometer's height: the ISA pressure altitude of the static pressure
// -- the measured pressure less the share of dynamic pressure the sensor
// sees, `airflow` -- plus the offset (weather, geoid) -- against the IMU's
// ellipsoidal height. That height is linearised about the prediction (h0 at
// p0, `up` the local vertical in ECEF): over a keyframe's worth of prediction
// error it is exact to the millimetre, and it spares csym a geodetic
// inversion.
constexpr auto kBaroHeight = [](auto p, auto v, auto o, auto pressure, auto p0, auto up, auto h0, auto rho,
                                auto inv_sigma, auto delta, auto eps) {
    using T = decltype(delta);
    const T offset = o[0], airflow = o[1];
    const T dynamic = T(0.5) * rho * (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const T static_pa = pressure - airflow * dynamic;
    const T pressure_altitude =
        T(isa::kT0 / isa::kLapse) * (T(1) - csym::exp(csym::log(static_pa / T(isa::kP0)) * T(1.0 / isa::kExponent)));
    const auto dp = p - p0;
    const T h = h0 + up[0] * dp[0] + up[1] * dp[1] + up[2] * dp[2];
    csym::Vector<T, 1> e;
    e[0] = (pressure_altitude + offset - h) * inv_sigma;
    return csym::PseudoHuberNoiseModel<T>(delta, T(1), eps).whiten_norm(e, eps);
};

using BaroHeightFactor =
    factor_graph::CsymFactor<kBaroHeight, factor_graph::Vars<V3, V3, V2>,
                             factor_graph::Params<double, V3, V3, double, double, double, double, double>>;

}  // namespace

FactorPtr baroHeight(Key p, Key v, Key barometer, double pressure_pa, const Eigen::Vector3d& p0_e,
                     const Eigen::Vector3d& up_e, double h0, double rho, double sigma, double robust_delta)
{
    return std::make_shared<BaroHeightFactor>("barometric height", std::array<Key, 3>{p, v, barometer}, pressure_pa,
                                              detail::toCsym3(p0_e), detail::toCsym3(up_e), h0, rho, 1.0 / sigma,
                                              robust_delta, factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
