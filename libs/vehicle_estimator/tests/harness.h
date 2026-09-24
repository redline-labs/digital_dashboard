#pragma once

// Runs a simulated drive through an Estimator the way the node does --
// messages in arrival order, process() after each -- and scores every
// keyframe against the truth.

#include "vehicle_estimator/estimator.h"
#include "vehicle_estimator/sim/scenario.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace harness
{

inline int failures = 0;

inline void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

constexpr double kDeg = std::numbers::pi / 180.0;

inline double wrap(double a)
{
    return std::remainder(a, 2.0 * std::numbers::pi);
}

struct Errors
{
    // Worst over the scored keyframes.
    double position = 0.0;  // m, horizontal and vertical together
    double velocity = 0.0;  // m/s
    double roll = 0.0, pitch = 0.0, yaw = 0.0;  // rad
    double sideslip = 0.0;  // rad, where valid on both sides
    // Normalised errors: how many of their own sigmas the estimates are off.
    double worst_nees_position = 0.0;
    double worst_z_sideslip = 0.0;
    std::size_t scored = 0, sideslip_scored = 0;
    // Where the worst sideslip error was.
    double worst_sideslip_time = 0.0;
};

inline vehicle_estimator::EstimatorConfig configFor(const vehicle_estimator::sim::SensorModel& s)
{
    vehicle_estimator::EstimatorConfig c;
    c.R_b_i = s.R_b_i;
    // The mounting given here is the exact truth, so it is stated as known --
    // navigation yaw is good to a few hundredths of a degree, and any honest
    // mounting sigma would swamp it in the consistency test;
    // the calibration tests widen it where they perturb it.
    c.mounting_sigma = Eigen::Vector3d::Constant(1e-5);
    c.reference_point = s.reference_point;
    c.lever_arm = s.lever_arm;
    c.antenna2_lever_arm = s.antenna2_lever_arm;
    c.dv_frame = s.dv_frame;
    // The one thing arrival-time alignment cannot see is the difference
    // between the two streams' latency floors; on the car it is calibrated,
    // here it is known.
    c.imu_time_offset = s.gnss_latency - s.imu_latency;
    return c;
}

struct Run
{
    Errors errors;
    vehicle_estimator::EstimatorStatus status;
    std::vector<vehicle_estimator::VehicleState> states;  // one per keyframe
    std::vector<vehicle_estimator::sim::Truth> truths;
    std::size_t claims_without_covariance = 0;
};

// Scores keyframes after `settle` seconds of scenario time.
inline Run run(const vehicle_estimator::sim::Scenario& sc, vehicle_estimator::Estimator& est, double settle,
               double min_speed_for_sideslip = 3.0)
{
    Run r;
    std::uint64_t seen_keyframes = 0;
    for (const auto& m : sc.messages())
    {
        if (m.imu) est.addImu(*m.imu);
        if (m.gnss) est.addGnss(*m.gnss);
        est.process();
        if (est.status().keyframes == seen_keyframes) continue;
        seen_keyframes = est.status().keyframes;
        const auto s = est.keyframeState();
        if (!s) continue;
        const double t = sc.toScenarioTime(s->gps_time);
        const auto truth = sc.truth(t);
        r.states.push_back(*s);
        r.truths.push_back(truth);
        // Every run, every keyframe: no claim without a covariance behind it.
        // Sigmas read zero when the window has none, and zero passes a bound.
        if ((s->attitude_valid || s->valid) && !(s->sigma_attitude.minCoeff() > 0.0)) ++r.claims_without_covariance;
        if (t < settle) continue;

        Errors& e = r.errors;
        ++e.scored;
        e.position = std::max(e.position, (s->p_e - truth.p_e).norm());
        e.velocity = std::max(e.velocity, (s->v_ned - truth.v_ned).norm());
        e.roll = std::max(e.roll, std::fabs(wrap(s->roll - truth.roll)));
        e.pitch = std::max(e.pitch, std::fabs(wrap(s->pitch - truth.pitch)));
        e.yaw = std::max(e.yaw, std::fabs(wrap(s->yaw - truth.yaw)));
        const double speed = std::hypot(truth.v_body.x(), truth.v_body.y());
        if (s->sideslip_valid && speed > min_speed_for_sideslip)
        {
            ++e.sideslip_scored;
            const double err = std::fabs(wrap(s->sideslip - truth.sideslip));
            if (err > e.sideslip)
            {
                e.sideslip = err;
                e.worst_sideslip_time = t;
            }
            if (s->sigma_sideslip > 0.0) e.worst_z_sideslip = std::max(e.worst_z_sideslip, err / s->sigma_sideslip);
        }
    }
    r.status = est.status();
    check(r.claims_without_covariance == 0,
          std::to_string(r.claims_without_covariance) + " keyframes claimed a valid attitude with no covariance");
    return r;
}

inline void report(const std::string& label, const Run& r)
{
    const auto& e = r.errors;
    SPDLOG_INFO("{}: {} keyframes scored; worst position {:.3f} m, velocity {:.3f} m/s, roll {:.3f} deg, pitch {:.3f} "
                "deg, yaw {:.3f} deg, sideslip {:.3f} deg ({} scored, worst at t={:.1f}, {:.1f} sigma)",
                label, e.scored, e.position, e.velocity, e.roll / kDeg, e.pitch / kDeg, e.yaw / kDeg,
                e.sideslip / kDeg, e.sideslip_scored, e.worst_sideslip_time, e.worst_z_sideslip);
    const auto& s = r.status;
    SPDLOG_INFO("{}: keyframes {} resets {} gated p/v/a {}/{}/{} refused {} imu bridged {} discarded {} gnss late {} "
                "timed out {}; lever arm [{:.3f} {:.3f} {:.3f}] boresight [{:.4f} {:.4f}] mounting [{:.3f} {:.3f} "
                "{:.3f}] deg sigma [{:.3f} {:.3f} {:.3f}] deg, {} segments, {} inertial keyframes, {} zero-velocity, solve {:.2f} ms",
                label, s.keyframes, s.resets, s.gated_position, s.gated_velocity, s.gated_attitude, s.updates_refused,
                s.imu_bridged, s.imu_discarded, s.gnss_late, s.gnss_timed_out, s.lever_arm.x(), s.lever_arm.y(),
                s.lever_arm.z(), s.boresight.x(), s.boresight.y(), s.mounting_rpy.x() / kDeg,
                s.mounting_rpy.y() / kDeg, s.mounting_rpy.z() / kDeg, s.mounting_sigma.x() / kDeg,
                s.mounting_sigma.y() / kDeg, s.mounting_sigma.z() / kDeg, s.calibration_segments, s.inertial_keyframes,
                s.zero_velocity_updates, s.last_solve_ms);
}

}  // namespace harness
