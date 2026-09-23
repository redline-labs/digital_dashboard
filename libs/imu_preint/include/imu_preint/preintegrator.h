#pragma once

// IMU preintegration between two keyframes (Forster et al., "On-Manifold
// Preintegration for Real-Time Visual-Inertial Odometry", TRO 2017),
// accumulated from strapdown increments instead of rates.
//
// The result is frame-free: rotation, velocity and position changes in the
// body frame of the first keyframe, computed at a fixed bias guess, plus the
// first-order Jacobians that let a later bias estimate correct them without
// re-integrating, and the covariance of the whole from the sensor's noise
// densities. Gravity and the earth's rotation are applied when the result is
// compared against two states (imu_factor.h), because both depend on where
// the states are.

#include "imu_preint/increment.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <string>

namespace imu_preint
{

struct NoiseParams
{
    // White noise densities. The MTi-610 datasheet gives 0.007 deg/s/sqrt(Hz)
    // for the gyroscope and 60 micro-g/sqrt(Hz) for the accelerometer.
    double gyro_noise_density = 0.007 * 3.14159265358979323846 / 180.0;  // rad/s/sqrt(Hz)
    double accel_noise_density = 60e-6 * 9.80665;                          // m/s^2/sqrt(Hz)
};

using Matrix9d = Eigen::Matrix<double, 9, 9>;

struct Preintegrated
{
    double dt = 0.0;
    Eigen::Matrix3d dR = Eigen::Matrix3d::Identity();
    Eigen::Vector3d dv = Eigen::Vector3d::Zero();
    Eigen::Vector3d dp = Eigen::Vector3d::Zero();

    // d(dR)/d(bg) in dR's tangent space, and the velocity and position
    // Jacobians, at the bias the increments were corrected with.
    Eigen::Matrix3d dR_dbg = Eigen::Matrix3d::Zero();
    Eigen::Matrix3d dv_dbg = Eigen::Matrix3d::Zero();
    Eigen::Matrix3d dv_dba = Eigen::Matrix3d::Zero();
    Eigen::Matrix3d dp_dbg = Eigen::Matrix3d::Zero();
    Eigen::Matrix3d dp_dba = Eigen::Matrix3d::Zero();
    Eigen::Vector3d bg_lin = Eigen::Vector3d::Zero();
    Eigen::Vector3d ba_lin = Eigen::Vector3d::Zero();

    // Covariance of [rotation, velocity, position] errors, in that order.
    Matrix9d cov = Matrix9d::Zero();

    std::size_t samples = 0;
    std::size_t bridged = 0;  // increments integrated to fill a gap, see integrate()
};

class Preintegrator
{
  public:
    Preintegrator(NoiseParams noise, DvFrame frame, const Eigen::Vector3d& bg_lin = Eigen::Vector3d::Zero(),
                  const Eigen::Vector3d& ba_lin = Eigen::Vector3d::Zero());

    // Adds one increment. extra_rot_var and extra_vel_var (rad^2, (m/s)^2 per
    // axis) inflate this increment's noise beyond the sensor's: that is how
    // a dropped sample is bridged -- the caller integrates a stand-in for it
    // and says how little it trusts the stand-in. Returns an error string,
    // leaving the state untouched, for an increment incrementProblem()
    // rejects.
    std::string integrate(const Increment& inc, double extra_rot_var = 0.0, double extra_vel_var = 0.0,
                          bool bridged = false);

    // Starts a new interval at a new bias guess.
    void reset(const Eigen::Vector3d& bg_lin, const Eigen::Vector3d& ba_lin);

    const Preintegrated& result() const { return p_; }
    DvFrame frame() const { return frame_; }

  private:
    void translate(const Eigen::Matrix3d& R, const Eigen::Vector3d& u, double dt, double var_u,
                   const Eigen::Matrix3d& R_start, const Eigen::Matrix3d& R_start_dbg);
    void rotate(const Eigen::Vector3d& theta, double dt, double var_theta);

    NoiseParams noise_;
    DvFrame frame_;
    Preintegrated p_;
    // The previous sample's specific force, in the body frame at its end --
    // which is also the frame the next sample starts in, across a reset().
    // It gives the position integral its slope within a sample; see
    // translate().
    Eigen::Vector3d f_prev_ = Eigen::Vector3d::Zero();
    Eigen::Matrix3d df_prev_dbg_ = Eigen::Matrix3d::Zero();
    Eigen::Matrix3d df_prev_dba_ = Eigen::Matrix3d::Zero();
    bool have_prev_ = false;
};

// SO(3) right Jacobian, Jr(theta): Exp(theta + d) = Exp(theta) Exp(Jr(theta) d).
Eigen::Matrix3d rightJacobian(const Eigen::Vector3d& theta);

Eigen::Matrix3d skew(const Eigen::Vector3d& v);

}  // namespace imu_preint
