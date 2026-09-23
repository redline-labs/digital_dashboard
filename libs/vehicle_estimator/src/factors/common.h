#pragma once

// Conversions shared by the factor translation units.

#include "csym/geo/rot3.h"
#include "csym/matrix.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace vehicle_estimator::factors::detail
{

template <std::size_t R, std::size_t C>
csym::Matrix<double, R, C> toCsym(const Eigen::MatrixXd& m)
{
    csym::Matrix<double, R, C> out;
    for (std::size_t r = 0; r < R; ++r)
        for (std::size_t c = 0; c < C; ++c) out(r, c) = m(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c));
    return out;
}

inline csym::Vector3<double> toCsym3(const Eigen::Vector3d& v)
{
    return csym::Vector3<double>{v.x(), v.y(), v.z()};
}

inline csym::Rot3<double> toCsymRot(const Eigen::Quaterniond& q)
{
    const Eigen::Quaterniond n = q.normalized();
    return csym::Rot3<double>(n.x(), n.y(), n.z(), n.w());
}

}  // namespace vehicle_estimator::factors::detail
