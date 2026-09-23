#pragma once

// One IMU sample as a strapdown increment: the rotation and the specific-
// force velocity change over the sample interval, already coning- and
// sculling-compensated by the sensor (the MTi's SDI outputs, XDI 0x8030 and
// 0x4010). Integrating these rather than rates is what keeps a fast yaw
// transient from turning into a heading error: the sensor integrated it at
// 2 kHz internally and handed over the exact result.

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <utility>

namespace imu_preint
{

// Which body frame dv is expressed in. The two differ by the rotation within
// one sample -- 0.01 rad at 1 rad/s and 100 Hz -- times dv, which adds up to
// a systematic velocity error over a keyframe interval, so it is a setting
// rather than an assumption. Xsens' reference manual writes the SDI
// velocity update as v_k = v_(k-1) + q_k * dv_k * q_k^-1 - g dt, i.e. the
// frame at the END of the interval. Not yet confirmed on a device.
enum class DvFrame
{
    start,
    end,
};

struct Increment
{
    double dt = 0.0;                                     // s
    Eigen::Quaterniond dq = Eigen::Quaterniond::Identity();  // body(k-1) -> body(k)
    Eigen::Vector3d dv = Eigen::Vector3d::Zero();        // m/s, specific force integral
};

// Rotation vector of an increment.
Eigen::Vector3d rotationVector(const Eigen::Quaterniond& q);
Eigen::Quaterniond fromRotationVector(const Eigen::Vector3d& v);

// Splits an increment at `fraction` of its interval, assuming constant
// angular rate and specific force within it -- how a keyframe that falls
// inside an IMU sample gets the part of it that belongs to each side. The
// dv halves are expressed in the frame `frame` of their own sub-interval.
std::pair<Increment, Increment> split(const Increment& inc, double fraction, DvFrame frame);

// Why an increment cannot be integrated, or empty. Non-finite values, a
// non-positive or implausibly long dt, and a quaternion too far from unit
// length to be a rotation (within 1e-3 it is renormalised instead).
const char* incrementProblem(const Increment& inc);

}  // namespace imu_preint
