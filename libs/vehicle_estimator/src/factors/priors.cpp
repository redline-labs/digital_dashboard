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

constexpr auto kPriorRot = [](auto R, auto mean, auto sqrt_info, auto eps) {
    return sqrt_info * csym::local_coordinates(mean, R, eps);
};
constexpr auto kPriorVec = [](auto x, auto mean, auto sqrt_info) { return sqrt_info * (x - mean); };

using PriorRotFactor = factor_graph::CsymFactor<kPriorRot, factor_graph::Vars<csym::Rot3<double>>,
                                                factor_graph::Params<csym::Rot3<double>, csym::Matrix33<double>, double>>;
using PriorV3Factor =
    factor_graph::CsymFactor<kPriorVec, factor_graph::Vars<V3>, factor_graph::Params<V3, csym::Matrix33<double>>>;
using PriorV2Factor =
    factor_graph::CsymFactor<kPriorVec, factor_graph::Vars<V2>, factor_graph::Params<V2, csym::Matrix22<double>>>;

}  // namespace

FactorPtr priorRot(Key R, const Eigen::Quaterniond& mean, const Eigen::Matrix3d& cov)
{
    return std::make_shared<PriorRotFactor>("attitude prior", std::array<Key, 1>{R}, detail::toCsymRot(mean),
                                            detail::toCsym<3, 3>(sqrtInformation(cov)), factor_graph::kEpsilon);
}

FactorPtr priorV3(Key x, const Eigen::Vector3d& mean, const Eigen::Matrix3d& cov, const char* name)
{
    return std::make_shared<PriorV3Factor>(name, std::array<Key, 1>{x}, detail::toCsym3(mean),
                                           detail::toCsym<3, 3>(sqrtInformation(cov)));
}

FactorPtr priorV2(Key x, const Eigen::Vector2d& mean, const Eigen::Matrix2d& cov, const char* name)
{
    return std::make_shared<PriorV2Factor>(name, std::array<Key, 1>{x}, V2{mean.x(), mean.y()},
                                           detail::toCsym<2, 2>(sqrtInformation(cov)));
}

}  // namespace vehicle_estimator::factors
