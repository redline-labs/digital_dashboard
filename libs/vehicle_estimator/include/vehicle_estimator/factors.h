#pragma once

// The measurement factors, each built in its own translation unit. Every
// residual is whitened by the measurement's covariance and then passed
// through a pseudo-Huber loss on its whitened norm -- quadratic out to
// `robust_delta` sigmas, linear beyond -- so one multipath jump pulls on the
// solution with bounded force instead of dragging it.
//
// Variables: R = R_e_i (Rot3), p and v = IMU position and velocity in ECEF,
// la = IMU-to-antenna-1 lever arm in the IMU frame, bs = boresight (2-vector).

#include "vehicle_estimator/calibration.h"

#include "factor_graph/factor.h"
#include "factor_graph/key.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <memory>

namespace vehicle_estimator::factors
{

using factor_graph::Key;
using FactorPtr = std::shared_ptr<const factor_graph::Factor>;

// Antenna 1 at p + R la; the receiver's position, in ECEF.
FactorPtr gnssPosition(Key R, Key p, Key la, const Eigen::Vector3d& measured_e, const Eigen::Matrix3d& cov_e,
                       double robust_delta);

// Antenna 1 moves at v + R (omega x la), omega the IMU's angular rate
// relative to the earth, in the IMU frame, held at its measured value.
FactorPtr gnssVelocity(Key R, Key v, Key la, const Eigen::Vector3d& measured_e, const Eigen::Matrix3d& cov_e,
                       const Eigen::Vector3d& omega_i, double robust_delta);

// The antenna baseline, in the IMU frame: its nominal direction b0 and two
// unit vectors u1, u2 perpendicular to it. The boresight bs tilts it to
// normalize(b0 + bs[0] u1 + bs[1] u2).
struct Baseline
{
    Eigen::Vector3d b0 = Eigen::Vector3d::UnitX();
    Eigen::Vector3d u1 = Eigen::Vector3d::UnitY();
    Eigen::Vector3d u2 = Eigen::Vector3d::UnitZ();

    static Baseline fromAntennas(const Eigen::Vector3d& antenna1_i, const Eigen::Vector3d& antenna2_i);
    Eigen::Vector3d direction(const Eigen::Vector2d& bs) const;
};

// The baseline's yaw and pitch in the local NED frame, R_n_e at the vehicle.
FactorPtr dualAntenna(Key R, Key bs, const Eigen::Matrix3d& R_n_e, const Baseline& baseline, double yaw,
                      double pitch, const Eigen::Matrix2d& cov, double robust_delta);

// The body moves along its own x axis: [sideslip, heave angle] of the
// reference point's velocity, through the mounting m = R_b_i. Only for
// keyframes the estimator has judged straight and true (see config.h).
FactorPtr straightDriving(Key R, Key v, Key mounting, const Eigen::Vector3d& omega_i, const Eigen::Vector3d& r_ref,
                          double sigma_slip, double sigma_heave, double robust_delta);

// A parked body is level: [roll, pitch] of R_n_e R_e_i R_b_i^T.
FactorPtr stationaryLevel(Key R, Key mounting, const Eigen::Matrix3d& R_n_e, double sigma, double robust_delta);

// The installation's first segment, from its prior (config, database, or
// what a previous run of the smoother learned).
FactorPtr calibrationPrior(const CalibrationKeys& keys, const CalibrationSet& prior);

// Segment to segment: a random walk of each part at its own density.
FactorPtr calibrationWalk(const CalibrationKeys& from, const CalibrationKeys& to, double dt, double mounting_walk,
                          double lever_arm_walk, double boresight_walk);

FactorPtr priorRot(Key R, const Eigen::Quaterniond& mean, const Eigen::Matrix3d& cov);
FactorPtr priorV3(Key x, const Eigen::Vector3d& mean, const Eigen::Matrix3d& cov, const char* name);
FactorPtr priorV2(Key x, const Eigen::Vector2d& mean, const Eigen::Matrix2d& cov, const char* name);

// S with S^T S = cov^-1; throws for a covariance that is not positive definite.
Eigen::MatrixXd sqrtInformation(const Eigen::MatrixXd& cov);

}  // namespace vehicle_estimator::factors
