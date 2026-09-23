#pragma once

// Truth for tests: a vehicle trajectory, and the exact strapdown increments
// an ideal IMU riding it would report -- including the earth's rotation,
// which a stationary gyro sees as 15 deg/h.
//
// The increments are built from inertial-frame quantities, not from the ECEF
// navigation equations the estimator uses, so a sign error in imu_model.h
// cannot be reproduced here and cancel:
//
//   dq = R_ib(t0)^T R_ib(t1),                R_ib(t) = Exp(omega t) R_eb(t)
//   dv = R_ib(t*)^T  integral R_ie(t) f_e(t) dt,   f_e = a_e + 2 omega x v_e - g_e
//
// with t* the start or end of the interval per DvFrame, and the integral by
// Gauss-Legendre quadrature to well below a micrometre per second.

#include "imu_preint/increment.h"

#include "geodesy/gravity.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <vector>

namespace imu_preint
{

struct TruthSample
{
    Eigen::Matrix3d R_e_b = Eigen::Matrix3d::Identity();
    Eigen::Vector3d p_e = Eigen::Vector3d::Zero();
    Eigen::Vector3d v_e = Eigen::Vector3d::Zero();  // relative to ECEF
    Eigen::Vector3d a_e = Eigen::Vector3d::Zero();  // d(v_e)/dt, in ECEF
};

class Trajectory
{
  public:
    virtual ~Trajectory() = default;
    virtual TruthSample at(double t) const = 0;
};

// A trajectory written in the NED frame at a fixed origin and carried into
// ECEF rigidly. Subclasses give position, velocity and acceleration in that
// frame and the body attitude relative to it.
class LocalTrajectory : public Trajectory
{
  public:
    struct Local
    {
        Eigen::Vector3d p = Eigen::Vector3d::Zero();
        Eigen::Vector3d v = Eigen::Vector3d::Zero();
        Eigen::Vector3d a = Eigen::Vector3d::Zero();
        Eigen::Matrix3d R_n_b = Eigen::Matrix3d::Identity();
    };

    LocalTrajectory(double lat_rad, double lon_rad, double h_m);
    TruthSample at(double t) const final;
    virtual Local local(double t) const = 0;

    const Eigen::Matrix3d& rotEcefFromNed() const { return r_e_n_; }
    const Eigen::Vector3d& originEcef() const { return origin_e_; }

  private:
    Eigen::Matrix3d r_e_n_;
    Eigen::Vector3d origin_e_;
};

// Increments for samples ending at t0 + k/rate_hz, k = 1..count.
std::vector<Increment> simulateIncrements(const Trajectory& traj, double t0, double rate_hz, std::size_t count,
                                          DvFrame frame, const geodesy::GravityModel& gravity);

Eigen::Matrix3d yawPitchRoll(double yaw, double pitch, double roll);

}  // namespace imu_preint
