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
using V9 = csym::Vector<double, 9>;
using Rot = csym::Rot3<double>;

// The field the magnetometer should read: the earth's, B_e from WMM, into
// the IMU frame, through the soft iron (I + S) scaled by 1/F, plus the hard
// iron h. k = [h (3), S as xx yy zz xy xz yz (6)]. Everything the attitude
// and the calibration do is in here; a car's own field is not, which is why
// sigma is loose and the loss robust.
constexpr auto kMagnetometer = [](auto R, auto k, auto measured, auto B_e, auto inv_F, auto inv_sigma, auto delta,
                                  auto eps) {
    using T = decltype(delta);
    const auto b = R.inverse() * B_e;
    const T x = ((T(1) + k[3]) * b[0] + k[6] * b[1] + k[7] * b[2]) * inv_F + k[0];
    const T y = (k[6] * b[0] + (T(1) + k[4]) * b[1] + k[8] * b[2]) * inv_F + k[1];
    const T z = (k[7] * b[0] + k[8] * b[1] + (T(1) + k[5]) * b[2]) * inv_F + k[2];
    csym::Vector3<T> e{(x - measured[0]) * inv_sigma, (y - measured[1]) * inv_sigma, (z - measured[2]) * inv_sigma};
    return csym::PseudoHuberNoiseModel<T>(delta, T(1), eps).whiten_norm(e, eps);
};

using MagnetometerFactor =
    factor_graph::CsymFactor<kMagnetometer, factor_graph::Vars<Rot, V9>,
                             factor_graph::Params<V3, V3, double, double, double, double>>;

}  // namespace

FactorPtr magnetometer(Key R, Key calibration, const Eigen::Vector3d& measured_au, const Eigen::Vector3d& field_e_nt,
                       double intensity_nt, double sigma_au, double robust_delta)
{
    return std::make_shared<MagnetometerFactor>("magnetometer", std::array<Key, 2>{R, calibration},
                                                detail::toCsym3(measured_au), detail::toCsym3(field_e_nt),
                                                1.0 / intensity_nt, 1.0 / sigma_au, robust_delta,
                                                factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
