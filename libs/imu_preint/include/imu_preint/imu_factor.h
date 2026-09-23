#pragma once

// The factors the IMU contributes to the graph: one preintegrated interval
// between two keyframes, and the random walk that ties consecutive biases.
// Each keyframe is five variables -- attitude R_e_b (Rot3), position p_e,
// velocity v_e, gyro bias and accelerometer bias (Vector3 each).

#include "imu_preint/preintegrator.h"

#include "factor_graph/factor.h"

#include <Eigen/Core>

#include <memory>

namespace imu_preint
{

struct KeyframeKeys
{
    factor_graph::Key R = 0;
    factor_graph::Key p = 0;
    factor_graph::Key v = 0;
    factor_graph::Key bg = 0;
    factor_graph::Key ba = 0;
};

// Residual [rotation, velocity, position] in body frame i, whitened by the
// preintegration covariance. gravity_e is evaluated at keyframe i's position
// estimate and held constant (see geodesy::GravityModel for why that is
// safe); omega_ie is the earth rate in ECEF.
std::shared_ptr<const factor_graph::Factor> makeImuFactor(const KeyframeKeys& i, const KeyframeKeys& j,
                                                          const Preintegrated& pim, const Eigen::Vector3d& gravity_e,
                                                          const Eigen::Vector3d& omega_ie);

// (b_j - b_i) / (sigma sqrt(dt)) per axis: a bias random walk of density
// `sigma` (units of the bias per sqrt(s)) over dt seconds.
std::shared_ptr<const factor_graph::Factor> makeBiasWalkFactor(factor_graph::Key bi, factor_graph::Key bj,
                                                               double sigma, double dt, const char* name);

// The same prediction the factor makes, for runtime propagation.
struct NavState
{
    Eigen::Quaterniond R_e_b = Eigen::Quaterniond::Identity();
    Eigen::Vector3d p_e = Eigen::Vector3d::Zero();
    Eigen::Vector3d v_e = Eigen::Vector3d::Zero();
};

NavState predict(const NavState& i, const Eigen::Vector3d& bg, const Eigen::Vector3d& ba, const Preintegrated& pim,
                 const Eigen::Vector3d& gravity_e, const Eigen::Vector3d& omega_ie);

}  // namespace imu_preint
