// SPDX-License-Identifier: GPL-3.0-or-later
//
// Learning how the IMU sits in the body. The mounting's yaw is sideslip error
// one for one, and no amount of IMU and GNSS agreement can see it: both
// sensors are happy with any mounting. What pins it is an assumption about
// the car -- running straight, it runs true -- and a drift car breaks that
// assumption on purpose, so half of what is tested here is that the gate
// keeps a drift from teaching the mounting anything.

#include "harness.h"

#include "vehicle_estimator/calibration.h"

using harness::check;
using harness::kDeg;
namespace sim = vehicle_estimator::sim;

namespace
{

// The configured mounting, wrong by `rpy` about the body axes.
Eigen::Matrix3d perturbed(const Eigen::Matrix3d& R_b_i, const Eigen::Vector3d& rpy)
{
    const Eigen::Matrix3d d = (Eigen::AngleAxisd(rpy.z(), Eigen::Vector3d::UnitZ()) *
                               Eigen::AngleAxisd(rpy.y(), Eigen::Vector3d::UnitY()) *
                               Eigen::AngleAxisd(rpy.x(), Eigen::Vector3d::UnitX()))
                                  .toRotationMatrix();
    return d * R_b_i;
}

// Mounting error about the body axes: the rotation from the true mounting to
// the estimate, as a small-angle vector.
Eigen::Vector3d mountingError(const vehicle_estimator::Estimator& est, const Eigen::Matrix3d& truth)
{
    const Eigen::Matrix3d R = est.calibration()->mounting.toRotationMatrix();
    const Eigen::AngleAxisd d(R * truth.transpose());
    return d.angle() * d.axis();
}

// An IMU on its side: 120 deg about (1,1,1). Every half turn -- the default
// MTi mounting, and any quarter turn composed with it -- is its own inverse,
// and nearly so with a small error on top, so a factor that used the mounting
// backwards would pass on those unnoticed.
Eigen::Matrix3d onItsSide()
{
    return (Eigen::AngleAxisd(0.5 * std::numbers::pi, Eigen::Vector3d::UnitZ()) *
            Eigen::AngleAxisd(0.5 * std::numbers::pi, Eigen::Vector3d::UnitX()))
        .toRotationMatrix();
}

vehicle_estimator::EstimatorConfig learning(const sim::SensorModel& sensors, const Eigen::Vector3d& wrong_by)
{
    auto config = harness::configFor(sensors);
    config.R_b_i = perturbed(sensors.R_b_i, wrong_by);
    config.mounting_sigma = Eigen::Vector3d(2.0, 2.0, 3.0) * kDeg;
    return config;
}

// Worst sideslip error over keyframes on a straight in [from, to).
double straightSideslipError(const harness::Run& r, const sim::Scenario& sc, double from, double to = 1e9)
{
    double worst = 0.0;
    for (std::size_t k = 0; k < r.states.size(); ++k)
    {
        const auto& s = r.states[k];
        const auto& t = r.truths[k];
        const double t_s = sc.toScenarioTime(s.gps_time);
        if (t_s < from || t_s >= to || !s.sideslip_valid) continue;
        if (t.v_body.x() < 15.0 || std::fabs(t.rate_body.z()) > 0.01) continue;
        worst = std::max(worst, std::fabs(harness::wrap(s.sideslip - t.sideslip)));
    }
    return worst;
}

void testYawFromStraights()
{
    // Two degrees of mounting yaw: every straight reads two degrees of slip
    // that is not there, until the straights have taught it otherwise.
    sim::SensorModel sensors;
    sensors.seed = 21;
    sensors.R_b_i = onItsSide();
    sim::Scenario sc(sim::track(), sensors);
    const auto config = learning(sensors, Eigen::Vector3d(0.0, 0.0, 2.0 * kDeg));
    vehicle_estimator::Estimator est(config);
    const auto r = harness::run(sc, est, 10.0);
    harness::report("mounting yaw", r);

    const Eigen::Vector3d err = mountingError(est, sensors.R_b_i);
    // The launch, past 15 m/s but before the gate has held for its two
    // seconds: the mounting is still as configured.
    const double first = straightSideslipError(r, sc, 8.6, 10.0);
    const double last = straightSideslipError(r, sc, sc.duration() - 6.0);
    SPDLOG_INFO("mounting yaw: error [{:.3f} {:.3f} {:.3f}] deg after {:.0f} s of straights; straight sideslip error "
                "{:.3f} deg at first, {:.3f} deg on the last",
                err.x() / kDeg, err.y() / kDeg, err.z() / kDeg, r.status.mount_straight_s, first / kDeg, last / kDeg);
    check(r.status.mount_straight_s > 15.0, "the straights were recognised");
    check(std::fabs(err.z()) < 0.2 * kDeg, "mounting yaw learned to 0.2 deg");
    check(last < 0.3 * kDeg, "and the last straight reads no slip");
    check(first > 1.5 * kDeg, "which it did not before the straights had taught it");
    check(r.status.mounting_sigma.z() < 0.5 * kDeg && std::fabs(err.z()) < 3.0 * r.status.mounting_sigma.z() + 0.05 * kDeg,
          "with a sigma that covers the error");
}

void testRollPitchFromStops()
{
    sim::SensorModel sensors;
    sensors.seed = 22;
    sensors.R_b_i = onItsSide();
    sim::Scenario sc(sim::stopAndGo(), sensors);
    const auto config = learning(sensors, Eigen::Vector3d(2.0 * kDeg, -1.5 * kDeg, 0.0));
    vehicle_estimator::Estimator est(config);
    const auto r = harness::run(sc, est, 10.0);
    harness::report("mounting roll/pitch", r);
    const Eigen::Vector3d err = mountingError(est, sensors.R_b_i);
    SPDLOG_INFO("mounting roll/pitch: error [{:.3f} {:.3f} {:.3f}] deg after {} stops", err.x() / kDeg, err.y() / kDeg,
                err.z() / kDeg, r.status.mount_level_stops);
    check(r.status.mount_level_stops >= 6, "every stop counted");
    check(std::fabs(err.x()) < 0.5 * kDeg, "mounting roll learned to 0.5 deg");
    check(std::fabs(err.y()) < 0.5 * kDeg, "mounting pitch learned to 0.5 deg");
}

void testDriftTeachesNothing()
{
    // The mounting is right and the car drifts the whole time: 3 to 26 deg of
    // real slip. A gate that let this in would "learn" a mounting yaw of the
    // average slip angle.
    sim::SensorModel sensors;
    sensors.seed = 23;
    sim::Scenario sc(sim::skidpad(), sensors);
    const auto config = learning(sensors, Eigen::Vector3d::Zero());
    vehicle_estimator::Estimator est(config);
    const auto r = harness::run(sc, est, 10.0);
    harness::report("drift", r);
    const Eigen::Vector3d err = mountingError(est, sensors.R_b_i);
    SPDLOG_INFO("drift: mounting error [{:.3f} {:.3f} {:.3f}] deg, {:.1f} s judged straight", err.x() / kDeg,
                err.y() / kDeg, err.z() / kDeg, r.status.mount_straight_s);
    check(r.status.mount_straight_s == 0.0, "no moment of a drift is judged straight");
    check(std::fabs(err.z()) < 0.1 * kDeg, "and the mounting yaw is left where it was");
}

double knock(double walk, unsigned seed)
{
    // The IMU turns 1.5 deg in its mount a minute in. The walk is what lets
    // the estimate follow; without it, a minute of confident history holds
    // the old value.
    sim::SensorModel sensors;
    sensors.seed = seed;
    sensors.mount_step = sim::SensorModel::MountStep{60.0, 0.5, 1.5 * kDeg};
    sim::TrackParams tp;
    tp.laps = 5;
    sim::Scenario sc(sim::track(tp), sensors);
    auto config = learning(sensors, Eigen::Vector3d::Zero());
    config.mounting_walk = walk;
    vehicle_estimator::Estimator est(config);
    const auto r = harness::run(sc, est, 10.0);
    const Eigen::Vector3d err = mountingError(est, sensors.mountingAt(sc.duration()));
    SPDLOG_INFO("knock, walk {:.1e} rad/sqrt(s): mounting yaw error {:.3f} deg at the end", walk, err.z() / kDeg);
    return std::fabs(err.z());
}

void testWalkFollowsAKnock()
{
    const double followed = knock(2e-3, 24);
    const double held = knock(1e-7, 24);
    check(followed < 0.3 * kDeg, "with a walk, the estimate follows a knocked mount");
    // A constant compromises between the minute before the knock and the
    // minute after; it cannot follow.
    check(held > 0.4 * kDeg && held > 5.0 * followed, "without one it does not -- so the walk is what does it");
}

}  // namespace

int main()
{
    testYawFromStraights();
    testRollPitchFromStops();
    testDriftTeachesNothing();
    testWalkFollowsAKnock();
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("mounting: all passed");
    return 0;
}
