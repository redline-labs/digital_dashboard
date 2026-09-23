#pragma once

// Test trajectories, analytic in position, velocity, acceleration and
// attitude so the simulator has an exact truth to integrate.

#include "imu_preint/sim.h"

#include <cmath>
#include <numbers>

namespace test_traj
{

constexpr double kDeg = std::numbers::pi / 180.0;

// Parked, level, nose at 30 degrees, at 47 N 8 E.
class Stationary final : public imu_preint::LocalTrajectory
{
  public:
    Stationary() : LocalTrajectory(47.0 * kDeg, 8.0 * kDeg, 400.0) {}
    Local local(double) const override
    {
        Local l;
        l.R_n_b = imu_preint::yawPitchRoll(30.0 * kDeg, 0.0, 0.0);
        return l;
    }
};

// A drift on a skidpad: 25 m/s round a 50 m circle (1.27 g), the nose held
// `beta` inside the direction of travel, with body roll and pitch wobble and
// a little vertical motion so no axis is idle.
class Skidpad final : public imu_preint::LocalTrajectory
{
  public:
    explicit Skidpad(double beta_rad = 10.0 * kDeg, double speed = 25.0, double radius = 50.0)
        : LocalTrajectory(-33.9 * kDeg, 151.2 * kDeg, 40.0), beta_(beta_rad), v_(speed), r_(radius)
    {
    }
    Local local(double t) const override
    {
        const double w = v_ / r_, phi = 0.4 + w * t;
        Local l;
        l.p = {r_ * std::cos(phi), r_ * std::sin(phi), 0.5 * std::sin(0.7 * t)};
        l.v = {-v_ * std::sin(phi), v_ * std::cos(phi), 0.35 * std::cos(0.7 * t)};
        l.a = {-v_ * w * std::cos(phi), -v_ * w * std::sin(phi), -0.245 * std::sin(0.7 * t)};
        const double course = phi + std::numbers::pi / 2;
        l.R_n_b = imu_preint::yawPitchRoll(course - beta_, 0.02 * std::cos(2.0 * t), 0.05 * std::sin(3.0 * t));
        return l;
    }

  private:
    double beta_, v_, r_;
};

// Accelerating in a straight line while tumbling about all three axes.
class Tumble final : public imu_preint::LocalTrajectory
{
  public:
    Tumble() : LocalTrajectory(64.1 * kDeg, -21.9 * kDeg, 120.0) {}
    Local local(double t) const override
    {
        const Eigen::Vector3d dir = Eigen::Vector3d(0.6, 0.7, -0.1).normalized();
        Local l;
        l.p = dir * (10.0 * t + 1.5 * t * t);
        l.v = dir * (10.0 + 3.0 * t);
        l.a = dir * 3.0;
        l.R_n_b = imu_preint::yawPitchRoll(0.3 + 2.0 * t + 0.5 * t * t, 0.2 * std::sin(5.0 * t), 0.3 * std::sin(7.0 * t));
        return l;
    }
};

}  // namespace test_traj
