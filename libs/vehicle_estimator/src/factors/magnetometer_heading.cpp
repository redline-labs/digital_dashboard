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
using Rot = csym::Rot3<double>;

// Heading only: where the horizontal part of the (already calibrated) field
// points, in the local level frame, against where it should -- `declination`
// east of that frame's x. With no position there is no reference for the
// dip or the strength, and x is magnetic north (declination zero). The
// calibration is a parameter here, not a variable: nothing in this factor
// could say whether it is right.
constexpr auto kMagneticHeading = [](auto R, auto corrected_i, auto R_n_e, auto declination, auto inv_sigma,
                                     auto delta, auto eps) {
    using T = decltype(delta);
    const auto d = R_n_e * (R * corrected_i);
    const T heading = csym::atan2(d[1], d[0]);
    const T dh = heading - declination;
    csym::Vector<T, 1> e;
    e[0] = csym::atan2(csym::sin(dh), csym::cos(dh)) * inv_sigma;
    return csym::PseudoHuberNoiseModel<T>(delta, T(1), eps).whiten_norm(e, eps);
};

using MagneticHeadingFactor =
    factor_graph::CsymFactor<kMagneticHeading, factor_graph::Vars<Rot>,
                             factor_graph::Params<V3, csym::Matrix33<double>, double, double, double, double>>;

}  // namespace

FactorPtr magneticHeading(Key R, const Eigen::Vector3d& corrected_i, const Eigen::Matrix3d& R_n_e, double declination,
                          double sigma, double robust_delta)
{
    return std::make_shared<MagneticHeadingFactor>("magnetic heading", std::array<Key, 1>{R},
                                                   detail::toCsym3(corrected_i), detail::toCsym<3, 3>(R_n_e),
                                                   declination, 1.0 / sigma, robust_delta, factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
