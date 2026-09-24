// SPDX-License-Identifier: GPL-3.0-or-later
//
// When a learned calibration is written, and when an old one still applies.
// Pure decisions, no database: the hash that says a stored value was learned
// against the priors in the config today, the rule that writes a row only when
// the estimate has meaningfully moved or tightened, and how a stored value
// becomes the next session's prior.

#include "vehicle_estimator/calibration.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <numbers>
#include <string>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

namespace ve = vehicle_estimator;
using ve::CalibrationGroup;
constexpr double kDeg = std::numbers::pi / 180.0;

ve::EstimatorConfig baseConfig()
{
    ve::EstimatorConfig c;
    c.lever_arm = Eigen::Vector3d(0.3, 0.0, 1.2);
    c.antenna2_lever_arm = Eigen::Vector3d(-1.2, 0.0, 1.2);
    return c;
}

void testHash()
{
    const auto c = baseConfig();
    const auto h = [&](CalibrationGroup g, const ve::EstimatorConfig& cfg) { return ve::priorHash(g, cfg); };

    // Golden: a hash that changed between builds or hosts would orphan every
    // row a car has ever written. If this fails and the change was meant,
    // bump the group's model version instead. The three values were computed
    // independently (Python, struct.pack('<d')) from the documented byte
    // layout, not copied from this code's output.
    check(ve::hashHex(h(CalibrationGroup::mounting, c)) == "34e0bbf45461ea0f",
          "mounting hash is the one on record: " + ve::hashHex(h(CalibrationGroup::mounting, c)));
    check(ve::hashHex(h(CalibrationGroup::lever_arm, c)) == "f36fa677ab23fffc",
          "lever-arm hash is the one on record: " + ve::hashHex(h(CalibrationGroup::lever_arm, c)));
    check(ve::hashHex(h(CalibrationGroup::boresight, c)) == "99c41a80afa4f452",
          "boresight hash is the one on record: " + ve::hashHex(h(CalibrationGroup::boresight, c)));

    // The magnetometer and the airflow, from non-zero priors so every value
    // reaches the bytes. Computed the same independent way.
    auto sensed = c;
    sensed.mag_hard_iron = Eigen::Vector3d(0.05, -0.08, 0.12);
    sensed.mag_soft_iron << 0.01, 0.02, -0.03, 0.004, 0.0, -0.005;
    sensed.baro_airflow = 0.3;
    check(ve::hashHex(h(CalibrationGroup::magnetometer, sensed)) == "2d5185dbce7e819c",
          "magnetometer hash is the one on record: " + ve::hashHex(h(CalibrationGroup::magnetometer, sensed)));
    check(ve::hashHex(h(CalibrationGroup::baro_airflow, sensed)) == "897a69a5a97ed238",
          "airflow hash is the one on record: " + ve::hashHex(h(CalibrationGroup::baro_airflow, sensed)));
    // Only its own prior invalidates each: the barometer's offset is weather,
    // not a stored group, and must orphan nothing.
    auto weather = sensed;
    weather.baro_offset = 55.0;
    weather.mag_sigma = 0.05;
    for (CalibrationGroup g : ve::kCalibrationGroups)
        check(h(g, weather) == h(g, sensed), std::string("the offset and sigmas leave the ") + std::string(ve::groupName(g)));
    auto soft = sensed;
    soft.mag_soft_iron[4] = 0.01;
    check(h(CalibrationGroup::magnetometer, soft) != h(CalibrationGroup::magnetometer, sensed),
          "an off-diagonal soft iron re-stated");
    check(h(CalibrationGroup::baro_airflow, soft) == h(CalibrationGroup::baro_airflow, sensed), "leaves the airflow");
    check(h(CalibrationGroup::mounting, sensed) == h(CalibrationGroup::mounting, c) &&
              h(CalibrationGroup::lever_arm, sensed) == h(CalibrationGroup::lever_arm, c),
          "and the magnetometer and airflow leave the installation's rows alone");
    for (CalibrationGroup g : ve::kCalibrationGroups)
        check(ve::groupName(g) != "baro_offset", "the barometer's offset is never a stored group");

    // Sigmas are not what was measured: changing them keeps what was learned.
    auto sure = c;
    sure.lever_arm_sigma = 0.005;
    sure.boresight_sigma = 0.1;
    sure.mounting_sigma = Eigen::Vector3d::Constant(0.2);
    for (CalibrationGroup g : ve::kCalibrationGroups)
        check(h(g, sure) == h(g, c), std::string("a sigma change keeps the ") + std::string(ve::groupName(g)));

    // A re-measured lever arm invalidates the lever arm AND the boresight
    // (learned around the baseline from it), not the mounting.
    auto remeasured = c;
    remeasured.lever_arm.x() += 0.01;
    check(h(CalibrationGroup::lever_arm, remeasured) != h(CalibrationGroup::lever_arm, c), "lever arm re-measured");
    check(h(CalibrationGroup::boresight, remeasured) != h(CalibrationGroup::boresight, c),
          "the boresight follows the lever arm");
    check(h(CalibrationGroup::mounting, remeasured) == h(CalibrationGroup::mounting, c), "the mounting does not");

    auto moved2 = c;
    moved2.antenna2_lever_arm.y() += 0.01;
    check(h(CalibrationGroup::boresight, moved2) != h(CalibrationGroup::boresight, c),
          "the second antenna moving invalidates the boresight");
    check(h(CalibrationGroup::lever_arm, moved2) == h(CalibrationGroup::lever_arm, c), "and not the lever arm");

    auto turned = c;
    turned.R_b_i = Eigen::AngleAxisd(0.5 * kDeg, Eigen::Vector3d::UnitZ()).toRotationMatrix() * c.R_b_i;
    check(h(CalibrationGroup::mounting, turned) != h(CalibrationGroup::mounting, c), "a re-stated mounting");
    check(h(CalibrationGroup::lever_arm, turned) == h(CalibrationGroup::lever_arm, c), "leaves the lever arm");

    // -0.0 and 0.0 are one measurement; a YAML "-0" must not orphan a row.
    auto negative_zero = c;
    negative_zero.lever_arm.y() = -0.0;
    check(h(CalibrationGroup::lever_arm, negative_zero) == h(CalibrationGroup::lever_arm, c), "-0.0 is 0.0");

    // Groups hash apart even from the same numbers.
    check(h(CalibrationGroup::mounting, c) != h(CalibrationGroup::lever_arm, c) &&
              h(CalibrationGroup::lever_arm, c) != h(CalibrationGroup::boresight, c),
          "the three groups hash apart");
}

ve::GroupEstimate lever(const Eigen::Vector3d& mean, const Eigen::Vector3d& sigma)
{
    return {CalibrationGroup::lever_arm, mean, Eigen::Matrix3d(sigma.cwiseAbs2().asDiagonal())};
}

ve::GroupEstimate mounting(double yaw, double sigma)
{
    const Eigen::Quaterniond q(Eigen::AngleAxisd(yaw, Eigen::Vector3d::UnitZ()));
    return {CalibrationGroup::mounting, Eigen::Vector4d(q.w(), q.x(), q.y(), q.z()),
            Eigen::Matrix3d(sigma * sigma * Eigen::Matrix3d::Identity())};
}

void testWritePolicy()
{
    const ve::WritePolicy p;
    ve::WriteContext ok;
    ok.now = 1000.0;
    ok.valid = true;
    ok.settled = 300.0;
    ok.evidence = 30.0;
    const auto cur = lever(Eigen::Vector3d(0.30, 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.05));
    using R = ve::WriteReason;

    check(ve::decideWrite(p, cur, std::nullopt, ok) == R::first_converged, "nothing written yet: write");
    auto c = ok;
    c.valid = false;
    check(!ve::decideWrite(p, cur, std::nullopt, c), "not while the solution is invalid");
    c = ok;
    c.settled = 60.0;
    check(!ve::decideWrite(p, cur, std::nullopt, c), "not within two minutes of a (re)start");
    c = ok;
    c.evidence = 0.0;
    check(ve::decideWrite(p, cur, std::nullopt, c) == R::first_converged, "the lever arm needs no evidence count");
    check(!ve::decideWrite(p, mounting(0.0, 0.01), std::nullopt, c), "a mounting nothing has taught is not written");

    // Moved: 1 sigma of the last written, whatever the units.
    const ve::LastWritten last{lever(Eigen::Vector3d(0.30, 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.05)), 0.0};
    check(!ve::decideWrite(p, lever(Eigen::Vector3d(0.302, 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.05)), last, ok),
          "2 mm against 3 mm has not moved");
    check(ve::decideWrite(p, lever(Eigen::Vector3d(0.304, 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.05)), last, ok) ==
              R::moved,
          "4 mm against 3 mm has");
    check(!ve::decideWrite(p, lever(Eigen::Vector3d(0.30, 0.0, 1.24), Eigen::Vector3d(0.003, 0.003, 0.05)), last, ok),
          "4 cm on an axis known to 5 cm has not: distance is in sigmas, not metres");
    const ve::LastWritten mlast{mounting(0.0, 0.1 * kDeg), 0.0};
    check(ve::decideWrite(p, mounting(0.15 * kDeg, 0.1 * kDeg), mlast, ok) == R::moved,
          "0.15 deg of mounting against 0.1 deg has moved");

    // Tightened: any axis to half its sigma.
    check(ve::decideWrite(p, lever(Eigen::Vector3d(0.30, 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.024)), last, ok) ==
              R::tightened,
          "the height known twice as well");
    check(!ve::decideWrite(p, lever(Eigen::Vector3d(0.30, 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.03)), last, ok),
          "but not merely somewhat better");

    // Rate limit, within a session.
    const auto far = lever(Eigen::Vector3d(0.31, 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.05));
    c = ok;
    c.now = last.at.value() + 600.0;
    check(!ve::decideWrite(p, far, last, c), "moved, but ten minutes after the last write: wait");
    c.now = last.at.value() + 901.0;
    check(ve::decideWrite(p, far, last, c) == R::moved, "fifteen minutes on: write");
    c.now = last.at.value() + 600.0;
    c.shutdown = true;
    check(ve::decideWrite(p, far, last, c) == R::shutdown_moved, "shutting down skips the wait");
    check(!ve::decideWrite(p, cur, last, c), "but not the need to have moved");
    // A row from an earlier session has no time on this clock: no wait.
    const ve::LastWritten stored{last.estimate, std::nullopt};
    check(ve::decideWrite(p, far, stored, ok) == R::moved, "a row from before this session is no reason to wait");
}

void testLoadPrior()
{
    const auto c = baseConfig();  // lever arm sigma 2 cm, boresight 0.02 rad
    // Inflated: sigma doubles for a factor of four.
    const auto stored = lever(Eigen::Vector3d(0.31, 0.0, 1.2), Eigen::Vector3d(0.003, 0.004, 0.005));
    const auto p = ve::loadPrior(stored, c, 4.0);
    check(p.mean == stored.mean, "the stored mean is the prior mean");
    check(std::fabs(std::sqrt(p.cov(0, 0)) - std::sqrt(4.0 * 0.003 * 0.003 + 0.002 * 0.002)) < 1e-12,
          "inflated, and floored");
    // Floored: a stored sigma of 0.1 mm does not come back as one.
    const auto tiny = ve::loadPrior(lever(stored.mean, Eigen::Vector3d::Constant(1e-4)), c, 4.0);
    check(std::sqrt(tiny.cov(0, 0)) > 0.002, "no prior tighter than the floor");
    // Capped: never looser than the config says.
    const auto loose = ve::loadPrior(lever(stored.mean, Eigen::Vector3d(0.01, 0.01, 0.05)), c, 4.0);
    check(loose.cov.diagonal().maxCoeff() <= 0.02 * 0.02 + 1e-15, "no prior looser than the tape measure");
    check(!ve::problem(loose), "and still a covariance");
}

void testMovedBetweenSessions()
{
    const auto loaded = lever(Eigen::Vector3d(0.30, 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.05));
    const auto now3 = lever(Eigen::Vector3d(0.30 + 3.0 * 0.003 * std::sqrt(2.0), 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.05));
    const auto now5 = lever(Eigen::Vector3d(0.30 + 5.0 * 0.003 * std::sqrt(2.0), 0.0, 1.2), Eigen::Vector3d(0.003, 0.003, 0.05));
    check(std::fabs(ve::distanceFrom(loaded, now3) - 3.0) < 1e-9, "3 sigma of the two together reads 3");
    check(ve::distanceFrom(loaded, now5) > 4.0, "5 reads past the 4 that says something moved");
}

void testGroupsAndProblems()
{
    ve::EstimatorConfig c = baseConfig();
    auto set = ve::configuredCalibration(c);
    set.cov(3, 6) = set.cov(6, 3) = 1e-6;  // a lever-arm/boresight correlation
    for (CalibrationGroup g : ve::kCalibrationGroups)
    {
        const auto e = ve::extract(set, g);
        check(!ve::problem(e), std::string(ve::groupName(g)) + " extracts cleanly");
        check(ve::groupFromName(ve::groupName(g)) == g, std::string(ve::groupName(g)) + " round-trips by name");
        check(!ve::summary(e).empty(), "and has a summary");
    }
    auto moved = ve::extract(set, CalibrationGroup::lever_arm);
    moved.mean.x() += 0.05;
    ve::apply(set, moved);
    check(set.lever_arm.x() == c.lever_arm.x() + 0.05, "apply sets the mean");
    check(set.cov(3, 6) == 0.0 && set.cov(6, 3) == 0.0, "and drops the correlation it cannot vouch for");
    check(!ve::groupFromName("tyre_pressure"), "an unknown group name is not a group");
    check(!ve::groupFromName("baro_offset"), "nor is the barometer's offset");

    // The magnetometer and the airflow land where the estimator reads them,
    // and apply() leaves the offset -- weather -- alone.
    auto mag = ve::extract(set, CalibrationGroup::magnetometer);
    check(mag.mean.size() == 9 && mag.cov.rows() == 9, "the magnetometer is nine values");
    mag.mean << 0.05, -0.08, 0.12, 0.01, 0.02, -0.03, 0.004, 0.0, -0.005;
    set.baro_offset = 42.0;
    ve::apply(set, mag);
    check((set.mag_hard_iron - Eigen::Vector3d(0.05, -0.08, 0.12)).norm() < 1e-15, "hard iron first");
    check(set.mag_soft_iron[2] == -0.03 && set.mag_soft_iron[5] == -0.005, "then soft iron xx yy zz xy xz yz");
    check((ve::extract(set, CalibrationGroup::magnetometer).mean - mag.mean).norm() < 1e-15, "and back");
    auto air = ve::extract(set, CalibrationGroup::baro_airflow);
    air.mean[0] = 0.3;
    air.cov(0, 0) = 0.01;
    ve::apply(set, air);
    check(set.baro_airflow == 0.3 && set.barometerCov()(1, 1) == 0.01, "the airflow is the barometer's second");
    check(set.baro_offset == 42.0 && set.barometerCov()(0, 0) == c.baro_offset_sigma * c.baro_offset_sigma,
          "and the offset beside it is untouched");

    auto bad = ve::extract(set, CalibrationGroup::mounting);
    bad.mean *= 1.1;
    check(ve::problem(bad).has_value(), "a quaternion that is not unit is refused");
    bad = ve::extract(set, CalibrationGroup::lever_arm);
    bad.cov(0, 0) = -1.0;
    check(ve::problem(bad).has_value(), "a covariance that is not positive definite is refused");
    bad = ve::extract(set, CalibrationGroup::lever_arm);
    bad.cov(0, 1) = 1e-3;
    check(ve::problem(bad).has_value(), "an asymmetric covariance is refused");
    bad = ve::extract(set, CalibrationGroup::boresight);
    bad.mean = Eigen::Vector3d::Zero();
    check(ve::problem(bad).has_value(), "a mean of the wrong size is refused");
    bad = ve::extract(set, CalibrationGroup::boresight);
    bad.mean[0] = std::nan("");
    check(ve::problem(bad).has_value(), "NaN is refused");
}

}  // namespace

int main()
{
    testHash();
    testWritePolicy();
    testLoadPrior();
    testMovedBetweenSessions();
    testGroupsAndProblems();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("calibration policy: all passed");
    return 0;
}
