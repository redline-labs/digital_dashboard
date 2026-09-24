#pragma once

// Everything about the estimator that is a property of the car or the
// sensors rather than of the algorithm. Defaults describe an MTi-610 mounted
// x-forward, z-up, and a BD992 on RTX.

#include "vehicle_estimator/measurements.h"

#include "factor_graph/optimizer.h"
#include "imu_preint/increment.h"
#include "imu_preint/preintegrator.h"

#include <Eigen/Core>

#include "geodesy/gravity.h"

#include <array>
#include <cstddef>
#include <memory>

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
    // Rotation from the IMU frame to the vehicle body (FRD), as measured;
    // estimated from here. The default is an MTi mounted x forward, z up: a
    // half turn about x.
    Eigen::Matrix3d R_b_i = (Eigen::Matrix3d() << 1, 0, 0, 0, -1, 0, 0, 0, -1).finished();
    // Of the measurement, about the body x, y, z (roll, pitch, yaw), rad.
    // Yaw is the one that matters: it is sideslip error one for one.
    Eigen::Vector3d mounting_sigma = Eigen::Vector3d(0.035, 0.035, 0.035);
    // Where sideslip and position are reported: the CG, or the rear axle. A
    // definition, not estimated: no kinematic measurement singles out a point.
    Eigen::Vector3d reference_point = Eigen::Vector3d::Zero();
    // IMU to the primary antenna, as measured; estimated from here.
    Eigen::Vector3d lever_arm = Eigen::Vector3d::Zero();
    double lever_arm_sigma = 0.02;  // m, per axis, of the measurement
    // IMU to the secondary antenna. Only its direction from the primary
    // matters -- it is the nominal baseline the boresight is estimated around.
    Eigen::Vector3d antenna2_lever_arm = Eigen::Vector3d(-1.0, 0.0, 0.0);
    double boresight_sigma = 0.02;  // rad, per axis

    // ---- learning the installation -----------------------------------------
    // The installation is a random walk, not a constant: one set of variables
    // per segment, joined by these densities (units per sqrt(s)). Slow enough
    // that nothing a car does in a lap moves it, fast enough that it never
    // hardens and can follow an antenna that was knocked.
    double calibration_segment = 1.0;       // s; at most half the lag
    double mounting_walk = 1.5e-5;          // rad/sqrt(s), 0.05 deg/sqrt(h)
    double lever_arm_walk = 8.3e-5;         // m/sqrt(s), 5 mm/sqrt(h)
    double boresight_walk = 1.5e-5;         // rad/sqrt(s)

    // What the mounting is learned from. Its roll and pitch: a parked body is
    // level, to within the grade -- weak alone, good averaged over stops that
    // face different ways. Its yaw and pitch: a body running straight and true
    // does not slide or heave. That second one is an ASSUMPTION about the
    // car, and a drift car breaks it on purpose, so its gate is strict and
    // held: no factor comes from a moment that merely looks straight.
    double straight_min_speed = 8.0;           // m/s
    double straight_max_yaw_rate = 0.026;      // rad/s (1.5 deg/s)
    double straight_max_lateral_accel = 0.5;   // m/s^2
    double straight_hold = 2.0;                // s the gate must hold first
    double straight_interval = 1.0;            // s between factors: the error is correlated
    double straight_sigma = 0.0087;            // rad (0.5 deg): crosswind, crown, toe
    double level_max_speed = 0.1;              // m/s
    double level_max_rate = 0.0087;            // rad/s (0.5 deg/s)
    double level_hold = 2.0;                   // s
    double level_interval = 10.0;              // s between factors in one long stop
    double level_sigma = 0.026;                // rad (1.5 deg): grade and camber

    // ---- magnetometer ----------------------------------------------------------
    // In the MTi's arbitrary units, IMU frame: m = A R_i_e B_e + h, with
    // A = (I + S) / F, F the local field intensity. Hard iron h and the
    // symmetric soft iron S (xx, yy, zz, xy, xz, yz) are learned, and kept
    // between sessions; these are their priors.
    bool use_magnetometer = true;
    Eigen::Vector3d mag_hard_iron = Eigen::Vector3d::Zero();
    double mag_hard_iron_sigma = 0.3;  // a.u.
    Eigen::Matrix<double, 6, 1> mag_soft_iron = Eigen::Matrix<double, 6, 1>::Zero();
    double mag_soft_iron_sigma = 0.1;
    double mag_hard_iron_walk = 0.01 / 60.0;   // a.u./sqrt(s): 0.01 per sqrt(h)
    double mag_soft_iron_walk = 0.005 / 60.0;  // per sqrt(s)
    // Per axis, a.u.: about 3 deg of heading at a mid-latitude dip. A car's
    // magnetometer is not better than that, whatever its datasheet says.
    double mag_sigma = 0.03;
    double mag_heading_sigma = 0.052;  // rad, the heading-only form, no position
    // A reading this many sigmas from the prediction is a disturbance -- a
    // bridge, a car alongside -- and not used. The sigma is the whole
    // prediction's: the attitude's, the calibration's, the sensor's.
    double mag_gate_sigmas = 5.0;
    // Seconds of learning against a dual-antenna heading before the
    // calibration is trusted to give a heading on its own -- this session's,
    // or a stored row's.
    double mag_trust_after = 30.0;

    // ---- barometer ---------------------------------------------------------------
    // h_ellipsoid = H_ISA(p - airflow * rho v^2 / 2) + offset. The offset is
    // the weather and the geoid, learned each session and never kept; the
    // airflow is where the sensor sits, learned and kept.
    bool use_barometer = true;
    double baro_offset = 0.0;           // m
    double baro_offset_sigma = 300.0;   // m
    double baro_offset_walk = 5.0 / 60.0;  // m/sqrt(s): 5 m per sqrt(h), the weather
    double baro_airflow = 0.0;
    double baro_airflow_sigma = 0.5;
    double baro_airflow_walk = 0.01 / 60.0;
    double baro_sigma = 0.5;  // m, per keyframe

    // ---- gravity ---------------------------------------------------------------
    // Where gravity comes from; null is WGS84 normal gravity. The node hands in
    // deflec::DeflectedGravity -- NGS's DEFLEC2022, loaded at run time -- which
    // leans gravity by the deflection of the vertical where the model covers:
    // over the contiguous US 6.6" RMS, up to ~45". Left out, that is ~3e-4
    // m/s^2 of horizontal acceleration the accelerometer bias absorbs only
    // while it stays put.
    std::shared_ptr<const geodesy::GravityModel> gravity;

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
        // The solve ends by TAKING a step this small (a tenth of a sigma),
        // so what is left is the step after it -- Gauss-Newton shrinks them
        // ~15x an iteration here, leaving well under a hundredth of a sigma.
        // Two solves a keyframe instead of three, with NEES unchanged.
        p.step_tolerance = 1e-1;
        // Gauss-Newton first. Marquardt damping is lambda * diag(H), and a
        // keyframe's diagonal is ~1e11 from the IMU chain while the window's
        // weak directions (all of it moving together) carry orders of
        // magnitude less: the usual 1e-6 damped exactly what the GNSS was
        // moving, and every keyframe ran to the iteration cap. A rejected
        // step still grows it.
        p.initial_lambda = 1e-14;
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
    // With no GNSS epoch due, a keyframe is made on the IMU's clock this
    // often: through an outage, and (with no fix yet) at a cold start.
    double keyframe_interval = 0.1;  // s
    // Parked: IMU still for this long AND the estimate's own speed below
    // the maximum. The IMU alone cannot tell parked from cruising straight.
    double zero_velocity_hold = 1.0;       // s
    // Tight: a car easing away from rest is still under 0.5 m/s for a second
    // while its IMU reads exactly what a parked one does.
    double zero_velocity_max_speed = 0.1;  // m/s
    double zero_velocity_sigma = 0.02;     // m/s
    // With no GNSS position this long after the IMU starts, start anyway --
    // anchored at a placeholder, attitude only -- and re-anchor at the first
    // fix. States are in ECEF, and without a position "level" means nothing
    // there; the anchor gives it a meaning until a real position does. The
    // anchor's gravity and earth rate are wrong by at most 0.3% and 15 deg/h,
    // which the biases absorb and relearn after the fix.
    bool anchored_start = true;
    double anchor_wait = 1.0;           // s
    double anchor_latitude = 0.785398;  // rad, 45 deg
    double anchor_longitude = 0.0;
};

}  // namespace vehicle_estimator
