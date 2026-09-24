// SPDX-License-Identifier: GPL-3.0-or-later
//
// A start with no GNSS: the same graph as an outage, anchored at a
// placeholder until the first fix. Attitude from gravity; heading from the
// magnetometer, MAGNETIC, and only from a calibration that was learned; then,
// at the first fix, one rotation into the true frame -- declination included
// -- and on as usual. With no learned calibration there is no heading to
// give, and the start waits for the antennas as it always did.

#include "harness.h"

#include "vehicle_estimator/calibration.h"
#include "vehicle_estimator/magnetic_reference.h"

#include "geodesy/geodetic.h"

using harness::check;
using harness::kDeg;
namespace sim = vehicle_estimator::sim;
namespace ve = vehicle_estimator;

namespace
{

// Parked with no GNSS for the first minute, then off.
std::unique_ptr<sim::VehicleMotion> parkedThenDrive()
{
    return sim::scripted({{70.0, 0.0}, {6.0, 15.0}, {40.0, 15.0, 2.0 * std::numbers::pi, 0.1}, {10.0, 15.0}});
}

double declinationAt(const sim::Scenario& sc)
{
    const auto llh = geodesy::ecefToLlh(csym::Vector3<double>{sc.truth(0.0).p_e.x(), sc.truth(0.0).p_e.y(),
                                                               sc.truth(0.0).p_e.z()});
    return ve::magneticField(llh.lat, llh.lon, llh.h, sc.sensors().gps_epoch).declination;
}

// The magnetometer calibration as the store would hand it over: learned in
// an earlier session, known to a hair.
ve::CalibrationSet learned(const ve::EstimatorConfig& config, const sim::SensorModel& s, const sim::Scenario& sc)
{
    ve::CalibrationSet c = ve::configuredCalibration(config);
    const auto llh = geodesy::ecefToLlh(csym::Vector3<double>{sc.truth(0.0).p_e.x(), sc.truth(0.0).p_e.y(),
                                                               sc.truth(0.0).p_e.z()});
    const double F = ve::magneticField(llh.lat, llh.lon, llh.h, s.gps_epoch).intensity_nt;
    const Eigen::Matrix3d S = s.mag_soft_iron * (F / s.mag_normalisation_nt) - Eigen::Matrix3d::Identity();
    c.mag_hard_iron = s.mag_hard_iron;
    c.mag_soft_iron << S(0, 0), S(1, 1), S(2, 2), S(0, 1), S(0, 2), S(1, 2);
    c.cov.block<9, 9>(8, 8) = 1e-6 * Eigen::Matrix<double, 9, 9>::Identity();
    return c;
}

struct Outcome
{
    bool anchored = false, attitude_valid = false, heading_magnetic = false, gps_time_valid = true;
    ve::HeadingSource source = ve::HeadingSource::none;
    double level_error = 0.0, magnetic_heading_error = 0.0, anchored_speed = 0.0;
    // The sigmas it claims while anchored: smallest and largest over the window.
    double sigma_min = 1e9, sigma_level_max = 0.0, sigma_yaw_max = 0.0;
    bool initialized_after = false;
    double yaw_error_after = 0.0;
    std::uint64_t reanchors = 0;
};

Outcome coldStart(sim::SensorModel sensors, bool seed_learned, double lat, double first_fix, double heading_from,
                  double yaw_window)
{
    sensors.outages = {{0.0, first_fix}};
    sensors.attitude_from = heading_from;
    sim::Scenario sc(parkedThenDrive(), sensors, lat, 8.57, 110.0);
    const auto config = harness::configFor(sensors);
    ve::Estimator est(config);
    if (seed_learned) est.seedCalibration(learned(config, sensors, sc), true);
    const double D = declinationAt(sc);

    Outcome o;
    for (const auto& m : sc.messages())
    {
        if (m.imu) est.addImu(*m.imu);
        if (m.gnss) est.addGnss(*m.gnss);
        est.process();
        const auto s = est.keyframeState();
        if (!s) continue;
        // Before any GNSS the estimator runs on host time and stamps its
        // states so; the window goes by the message instead.
        const double t_msg = m.host_time - sensors.host_epoch;
        if (t_msg > 20.0 && t_msg < first_fix - 1.0)
        {
            const auto truth = sc.truth(t_msg);  // parked: any instant near it
            o.anchored = est.status().anchored;
            o.attitude_valid = s->attitude_valid;
            o.heading_magnetic = s->heading_magnetic;
            o.gps_time_valid = s->gps_time_valid;
            o.sigma_min = std::min(o.sigma_min, s->sigma_attitude.minCoeff());
            o.sigma_level_max = std::max({o.sigma_level_max, s->sigma_attitude.x(), s->sigma_attitude.y()});
            o.sigma_yaw_max = std::max(o.sigma_yaw_max, s->sigma_attitude.z());
            o.source = s->heading_source;
            o.level_error = std::max({o.level_error, std::fabs(harness::wrap(s->roll - truth.roll)),
                                      std::fabs(harness::wrap(s->pitch - truth.pitch))});
            o.magnetic_heading_error =
                std::max(o.magnetic_heading_error, std::fabs(harness::wrap(s->yaw - (truth.yaw - D))));
            // Parked: the placeholder frame's velocity is still a velocity.
            o.anchored_speed = std::max(o.anchored_speed, s->v_ned.norm());
        }
        const double t = sc.toScenarioTime(s->gps_time);
        if (t > first_fix + 1.0 && t < first_fix + yaw_window && est.status().initialized)
        {
            const auto truth = sc.truth(t);
            o.initialized_after = true;
            o.yaw_error_after = std::max(o.yaw_error_after, std::fabs(harness::wrap(s->yaw - truth.yaw)));
        }
    }
    o.reanchors = est.status().reanchors;
    SPDLOG_INFO("cold start at {:.0f} deg lat (declination {:.1f} deg), {} calibration, fix at {:.0f} s, heading from "
                "{:.0f} s: anchored {}, attitude valid {}, magnetic {}, level error {:.3f} deg, magnetic heading "
                "error {:.2f} deg, speed {:.3f} m/s; after the fix: initialised {}, yaw error {:.2f} deg, {} re-anchors",
                lat, D / kDeg, seed_learned ? "learned" : "configured", first_fix, heading_from, o.anchored,
                o.attitude_valid, o.heading_magnetic, o.level_error / kDeg, o.magnetic_heading_error / kDeg,
                o.anchored_speed, o.initialized_after, o.yaw_error_after / kDeg, o.reanchors);
    return o;
}

sim::SensorModel sensors(unsigned seed)
{
    sim::SensorModel s;
    s.seed = seed;
    return s;
}

void testParkedWithLearnedMagnetometer()
{
    // No antennas' heading until well after the fix: the magnetometer is all.
    const Outcome o = coldStart(sensors(81), true, 49.33, 60.0, 90.0, 20.0);
    check(o.anchored, "with no GNSS, the estimator starts anchored");
    check(o.attitude_valid, "and its attitude is valid");
    check(!o.gps_time_valid, "stamped on the host clock, and saying so");
    check(o.heading_magnetic, "with the heading marked magnetic");
    check(o.source == ve::HeadingSource::magnetometer, "and coming from the magnetometer");
    check(o.level_error < 0.3 * kDeg, "roll and pitch from gravity to 0.3 deg");
    // A valid attitude with no covariance behind it once read sigma 0.
    check(o.sigma_min > 0.0, "with a covariance behind it, every keyframe");
    check(o.level_error < 3.0 * o.sigma_level_max && o.sigma_level_max < 1.0 * kDeg, "and sigmas that cover the error");
    check(o.magnetic_heading_error < 3.0 * o.sigma_yaw_max, "the heading's too");
    check(o.magnetic_heading_error < 3.0 * kDeg, "magnetic heading to 3 deg");
    check(o.anchored_speed < 0.05, "parked stays parked, even anchored");
    check(o.reanchors == 1 && o.initialized_after, "the first fix re-anchors, with no antennas' heading yet");
    check(o.yaw_error_after < 3.0 * kDeg, "and TRUE heading follows, declination and all");
}

void testReanchorWithDualAntenna()
{
    const Outcome o = coldStart(sensors(82), true, 49.33, 60.0, 0.0, 10.0);
    check(o.reanchors == 1 && o.initialized_after, "re-anchored at the fix");
    check(o.yaw_error_after < 0.5 * kDeg, "and the antennas' heading takes over at once");
}

void testConfiguredCalibrationWaits()
{
    // A hard iron nobody has learned: no heading to give. Roll and pitch are
    // still right; the start waits for the antennas, as it always did.
    const Outcome o = coldStart(sensors(83), false, 49.33, 60.0, 90.0, 20.0);
    check(o.anchored && o.attitude_valid, "anchored, attitude valid");
    check(o.source == ve::HeadingSource::none && !o.heading_magnetic, "but no heading");
    check(o.level_error < 0.3 * kDeg, "roll and pitch still from gravity");
    check(o.reanchors == 0, "no re-anchor without a heading to carry over");
    check(!o.initialized_after, "and no start until the antennas have one");
}

void testFarFromTheAnchor()
{
    // 60 deg S: gravity, earth rate and declination all unlike the anchor's.
    const Outcome o = coldStart(sensors(84), true, -60.0, 60.0, 90.0, 20.0);
    check(o.level_error < 0.3 * kDeg, "level far from the anchor");
    check(o.magnetic_heading_error < 3.0 * kDeg, "magnetic heading far from the anchor");
    check(o.reanchors == 1 && o.initialized_after && o.yaw_error_after < 3.0 * kDeg,
          "and the re-anchor lands true heading there too");
}

void testNoMagnetometer()
{
    auto s = sensors(85);
    s.magnetometer = false;
    const Outcome o = coldStart(s, true, 49.33, 60.0, 90.0, 20.0);
    check(o.source == ve::HeadingSource::none && !o.heading_magnetic, "no magnetometer, no heading");
}

}  // namespace

int main()
{
    testParkedWithLearnedMagnetometer();
    testReanchorWithDualAntenna();
    testConfiguredCalibrationWaits();
    testFarFromTheAnchor();
    testNoMagnetometer();
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("cold start: all passed");
    return 0;
}
