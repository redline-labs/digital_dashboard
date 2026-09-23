#include "vehicle_estimator/factors.h"

#include "common.h"

#include "csym/noise_models.h"
#include "factor_graph/csym_factor.h"
#include "factor_graph/values.h"

#include <Eigen/Cholesky>

#include <stdexcept>

namespace vehicle_estimator::factors
{

namespace
{

using V3 = csym::Vector3<double>;
using M33 = csym::Matrix33<double>;

constexpr auto kGnssPosition = [](auto R, auto p, auto la, auto measured, auto sqrt_info, auto delta, auto eps) {
    using T = decltype(delta);
    const auto r = sqrt_info * (p + R * la - measured);
    return csym::PseudoHuberNoiseModel<T>(delta, T(1), eps).whiten_norm(r, eps);
};

using GnssPositionFactor = factor_graph::CsymFactor<kGnssPosition, factor_graph::Vars<csym::Rot3<double>, V3, V3>,
                                                    factor_graph::Params<V3, M33, double, double>>;

}  // namespace

Eigen::MatrixXd sqrtInformation(const Eigen::MatrixXd& cov)
{
    const Eigen::LLT<Eigen::MatrixXd> llt(0.5 * (cov + cov.transpose()));
    if (llt.info() != Eigen::Success || !cov.allFinite())
        throw std::invalid_argument("covariance is not positive definite");
    const Eigen::MatrixXd l = llt.matrixL();
    return l.triangularView<Eigen::Lower>().solve(Eigen::MatrixXd::Identity(cov.rows(), cov.cols()));
}

FactorPtr gnssPosition(Key R, Key p, Key la, const Eigen::Vector3d& measured_e, const Eigen::Matrix3d& cov_e,
                       double robust_delta)
{
    return std::make_shared<GnssPositionFactor>("gnss position", std::array<Key, 3>{R, p, la},
                                                detail::toCsym3(measured_e),
                                                detail::toCsym<3, 3>(sqrtInformation(cov_e)), robust_delta,
                                                factor_graph::kEpsilon);
}

}  // namespace vehicle_estimator::factors
