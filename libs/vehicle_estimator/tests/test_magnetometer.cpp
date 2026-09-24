// SPDX-License-Identifier: GPL-3.0-or-later
//
// The magnetometer: its hard and soft iron learned while the antennas give
// a heading to learn against, and then what it is worth when they do not.
// A car's magnetometer is good to a few degrees and a learned gyro loses far
// less than that in a minute, so it earns its place only when the gyro bias
// wanders -- as an MTi's does, at its in-run stability -- over minutes
// without a heading. And a field that is not the earth's (a bridge, a car
// alongside) must be refused, not believed.

#include "harness.h"

#include "vehicle_estimator/calibration.h"
#include "vehicle_estimator/magnetic_reference.h"

#include "geodesy/geodetic.h"

using harness::check;
using harness::kDeg;
namespace sim = vehicle_estimator::sim;

namespace
{

// What the estimator's soft iron should converge to: the simulator's
// A_true scaled from its factory normalisation to the local field.
Eigen::Matrix3d softIronTruth(const sim::SensorModel& s, const sim::Scenario& sc)
{
    const auto llh = geodesy::ecefToLlh(csym::Vector3<double>{sc.truth(0.0).p_e.x(), sc.truth(0.0).p_e.y(),
                                                               sc.truth(0.0).p_e.z()});
    const auto field = vehicle_estimator::magneticField(llh.lat, llh.lon, llh.h, s.gps_epoch);
    return s.mag_soft_iron * (field.intensity_nt / s.mag_normalisation_nt);
}

double worstYaw(const harness::Run& r, const sim::Scenario& sc, double t0, double t1)
{
    double worst = 0.0;
    for (std::size_t k = 0; k < r.states.size(); ++k)
    {
        const double t = sc.toScenarioTime(r.states[k].gps_time);
        if (t >= t0 && t < t1) worst = std::max(worst, std::fabs(harness::wrap(r.states[k].yaw - r.truths[k].yaw)));
    }
    return worst;
}

void testLearned(const Eigen::Vector3d& hard_iron, unsigned seed, const char* label)
{
    sim::SensorModel sensors;
    sensors.seed = seed;
    sensors.mag_hard_iron = hard_iron;
    sim::SkidpadParams p;
    p.drive = 80.0;
    sim::Scenario sc(sim::skidpad(p), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    const auto& cal = *est.calibration();
    const Eigen::Matrix3d soft = cal.softIronMatrix(), truth = softIronTruth(sensors, sc);
    const Eigen::Vector3d dh = cal.mag_hard_iron - sensors.mag_hard_iron;
    SPDLOG_INFO("magnetometer, {}: {} used, {} rejected, {:.0f} s learning; hard iron error [{:.4f} {:.4f} {:.4f}], "
                "soft iron xx/yy/xy error {:.4f} {:.4f} {:.4f}",
                label, r.status.mag_used, r.status.mag_rejected, r.status.mag_learning_s, dh.x(), dh.y(), dh.z(),
                soft(0, 0) - truth(0, 0), soft(1, 1) - truth(1, 1), soft(0, 1) - truth(0, 1));
    check(r.status.mag_used > 700, "the magnetometer is used");
    check(r.status.mag_learning_s > 60.0, "with a dual-antenna heading to learn against");
    // The IMU's x and y are horizontal in this mounting; its z is the
    // vertical a car barely tilts away from, and is left to its prior.
    check(dh.head<2>().norm() < 0.01, "horizontal hard iron learned to 0.01 a.u.");
    check(std::fabs(soft(0, 0) - truth(0, 0)) < 0.02 && std::fabs(soft(1, 1) - truth(1, 1)) < 0.02 &&
              std::fabs(soft(0, 1) - truth(0, 1)) < 0.02,
          "horizontal soft iron learned to 0.02");
}

// The gyro bias wandering at an MTi's in-run stability, and from 60 s the
// heading (or all of GNSS) gone for `duration`. Driving laps, or parked --
// parked is where only the magnetometer can say where the car points: moving,
// GNSS velocity through the corners makes heading observable without it.
double withoutHeading(bool magnetometer, bool all_of_gnss, bool parked, double duration, unsigned seed,
                      const std::vector<sim::SensorModel::MagDisturbance>& disturbances = {})
{
    sim::SensorModel sensors;
    sensors.seed = seed;
    sensors.gyro_bias_walk = 2e-5;
    sensors.mag_disturbances = disturbances;
    if (all_of_gnss)
        sensors.outages = {{60.0, 60.0 + duration}};
    else
        sensors.attitude_outages = {{60.0, 60.0 + duration}};
    // Parked: a lap to learn the magnetometer on, then stopped in the pits.
    std::vector<sim::Phase> pits{{5.0, 0.0}, {6.0, 15.0}, {40.0, 15.0, 2.0 * std::numbers::pi, 0.1}, {6.0, 0.0},
                                 {duration + 10.0, 0.0}};
    sim::TrackParams tp;
    tp.laps = static_cast<int>((60.0 + duration) / 32.0) + 1;
    sim::Scenario sc(parked ? sim::scripted(pits) : sim::track(tp), sensors);
    auto config = harness::configFor(sensors);
    config.use_magnetometer = magnetometer;
    vehicle_estimator::Estimator est(config);
    const auto r = harness::run(sc, est, 10.0);
    const double worst = worstYaw(r, sc, 60.0, 60.0 + duration);
    SPDLOG_INFO("{:.0f} s {} without {}, magnetometer {}: worst yaw {:.3f} deg ({} rejected)", duration,
                parked ? "parked" : "driving", all_of_gnss ? "GNSS" : "a heading", magnetometer ? "on" : "off",
                worst / kDeg, r.status.mag_rejected);
    return worst;
}

void testHeadingDropout()
{
    // Driving: GNSS velocity through the corners carries the heading, and the
    // magnetometer adds nothing -- but must not cost anything either.
    const double driving_with = withoutHeading(true, false, false, 240.0, 72);
    const double driving_without = withoutHeading(false, false, false, 240.0, 72);
    check(driving_with < driving_without + 0.05 * kDeg, "driving without the antennas' heading: no worse for it");
    // Parked: nothing but the magnetometer says where the car points.
    const double parked_with = withoutHeading(true, false, true, 300.0, 72);
    const double parked_without = withoutHeading(false, false, true, 300.0, 72);
    check(parked_with * 2.0 < parked_without, "parked five minutes without the heading: held by the magnetometer");
}

void testOutage()
{
    const double with = withoutHeading(true, true, false, 300.0, 73);
    const double without = withoutHeading(false, true, false, 300.0, 73);
    check(with * 3.0 < without, "five minutes without GNSS: far better with the magnetometer");
}

void testDisturbance()
{
    // Thirty seconds beside something steel, parked without a heading, when
    // the magnetometer is all the heading there is.
    const std::vector<sim::SensorModel::MagDisturbance> steel{{150.0, 180.0, Eigen::Vector3d(0.35, 0.25, 0.1)}};
    const double clean = withoutHeading(true, false, true, 300.0, 74);
    const double disturbed = withoutHeading(true, false, true, 300.0, 74, steel);
    check(disturbed < clean + 0.3 * kDeg, "a disturbance is refused, not believed");
}

}  // namespace

int main()
{
    testLearned(Eigen::Vector3d(0.05, -0.08, 0.12), 71, "small hard iron");
    // Large: every reading fails a strict gate until something is learned,
    // so the gate must widen by what the calibration does not yet know.
    testLearned(Eigen::Vector3d(0.4, -0.3, 0.2), 75, "large hard iron");
    testHeadingDropout();
    testOutage();
    testDisturbance();
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("magnetometer: all passed");
    return 0;
}
