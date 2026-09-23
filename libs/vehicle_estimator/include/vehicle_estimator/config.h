#pragma once

// Everything about the estimator that is a property of the car or the
// sensors rather than of the algorithm. Defaults describe an MTi-610 mounted
// x-forward, z-up, and a BD992 on RTX.

#include "vehicle_estimator/measurements.h"

#include "factor_graph/optimizer.h"
#include "imu_preint/increment.h"
#include "imu_preint/preintegrator.h"

#include <Eigen/Core>

#include <array>
#include <cstddef>

namespace vehicle_estimator
{

struct EstimatorConfig
{
    // ---- IMU -----------------------------------------------------------------
    imu_preint::NoiseParams imu_noise;
    imu_preint::DvFrame dv_frame = imu_preint::DvFrame::end;
    // Bias random walk densities, bias units per sqrt(s).
    double gyro_bias_walk = 2e-5;
    double accel_bias_walk = 2e-4;
    // Turn-on bias uncertainty, for the first keyframe's prior.
    double gyro_bias_prior = 2e-3;   // rad/s
    double accel_bias_prior = 0.05;  // m/s^2
    // Samples a gap may lose and still be bridged; beyond it the estimator
    // re-initialises rather than guess the rotation it missed.
    std::size_t max_bridge_samples = 10;
    // Added to every IMU sample's mapped time: the difference between the
    // two sensors' minimum latencies, which host-arrival alignment cannot see.
    double imu_time_offset = 0.0;

    // ---- geometry, all in the IMU frame --------------------------------------
    // Rotation from the IMU frame to the vehicle body (FRD). The default is an
    // MTi mounted x forward, z up: a half turn about x.
    Eigen::Matrix3d R_b_i = (Eigen::Matrix3d() << 1, 0, 0, 0, -1, 0, 0, 0, -1).finished();
    // Where sideslip and position are reported: the CG, or the rear axle.
    Eigen::Vector3d reference_point = Eigen::Vector3d::Zero();
    // IMU to the primary antenna, as measured; estimated from here.
    Eigen::Vector3d lever_arm = Eigen::Vector3d::Zero();
    double lever_arm_sigma = 0.02;  // m, per axis, of the measurement
    // IMU to the secondary antenna. Only its direction from the primary
    // matters -- it is the nominal baseline the boresight is estimated around.
    Eigen::Vector3d antenna2_lever_arm = Eigen::Vector3d(-1.0, 0.0, 0.0);
    double boresight_sigma = 0.02;  // rad, per axis

    // ---- GNSS ----------------------------------------------------------------
    // Sigma multiplier per FixQuality (none, autonomous, differential, float,
    // rtx, fixed). The receiver's sigmas are already per-epoch; these are
    // floors on how much to believe them.
    std::array<double, 6> fix_sigma_scale{0.0, 3.0, 2.0, 1.5, 1.0, 1.0};
    double position_sigma_floor = 0.01;  // m
    // GSOF 8 carries no velocity sigma.
    double velocity_sigma_horizontal = 0.03;  // m/s
    double velocity_sigma_vertical = 0.06;    // m/s
    // Used when GSOF 27 arrives in its short form, without variances.
    double attitude_sigma_yaw = 0.0035;    // rad (0.2 deg)
    double attitude_sigma_pitch = 0.007;   // rad (0.4 deg)
    // Pseudo-Huber transition, in sigmas: quadratic inside, linear outside.
    double robust_delta = 3.0;
    // An innovation this many sigmas from the prediction is not used at all...
    double gate_sigmas = 30.0;
    // ...unless this many in a row have been: then the prediction, not the
    // measurement, is what is wrong, and gating against it would lock the
    // estimator out of the very data that could fix it. The next one goes in
    // (under the robust loss) and the count restarts.
    std::size_t gate_lockout = 5;

    // ---- smoother ------------------------------------------------------------
    double lag = 3.0;  // s
    factor_graph::LmParams lm = [] {
        factor_graph::LmParams p;
        p.max_iterations = 8;
        p.step_tolerance = 1e-4;
        return p;
    }();

    // ---- start-up and output -------------------------------------------------
    // Quasi-static enough to take roll from the accelerometer.
    double static_accel_tolerance = 0.3;  // m/s^2 from |g|
    double static_rate_tolerance = 0.05;  // rad/s
    double init_roll_sigma_moving = 0.17;  // rad, when roll cannot be levelled
    double init_velocity_sigma = 5.0;      // m/s, when no GNSS velocity
    // The state is reported valid below these one-sigma bounds.
    double valid_attitude_sigma = 0.035;  // rad
    double valid_position_sigma = 1.0;    // m
    // Below this speed sideslip is undefined and flagged invalid.
    double sideslip_min_speed = 2.0;  // m/s
    // How long a GNSS epoch waits for the IMU to cover it before it is dropped.
    double max_gnss_wait = 0.5;  // s
    // An epoch becomes a keyframe only once the IMU is this far past it, so
    // one that arrives after its successor (a receiver's latency jitters)
    // still gets its turn. Output latency is unaffected: between keyframes
    // the IMU carries the state forward anyway.
    double gnss_reorder_window = 0.15;  // s
};

}  // namespace vehicle_estimator
