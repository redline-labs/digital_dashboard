// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pairing the bridge's per-record GSOF topics into epochs. Every mistake here
// is a plausible wrong answer: a position stamped with the previous epoch's
// time is 2 m behind at 20 m/s, and nothing downstream can tell.

#include "gnss_assembler.h"

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

using namespace state_estimator;
constexpr double kDeg = std::numbers::pi / 180.0;

// One transmission's worth of records at `t` (arrival) for GPS ms `tow`.
void transmission(GnssAssembler& a, double t, std::uint32_t tow, bool with_time = true)
{
    // Position first, time after: the order a receiver's output list decides.
    a.addPosition({49.1, 8.5, 110.0}, t);
    a.addVelocity({true, 20.0, 90.0, 0.5}, t + 0.0002);
    if (with_time) a.addTime({2400, tow}, t + 0.0004);
    AttitudeRecord att;
    att.timeOfWeekMs = tow;
    att.pitchValid = att.yawValid = true;
    att.yawDeg = 91.0;
    att.pitchDeg = -1.0;
    a.addAttitude(att, t + 0.0006);
}

void testBurst()
{
    GnssAssembler a;
    transmission(a, 100.0, 1000);
    check(a.poll(100.005).empty(), "not closed while the burst could still grow");
    const auto epochs = a.poll(100.05);
    check(epochs.size() == 1, "one epoch from one transmission");
    if (epochs.empty()) return;
    const auto& e = epochs[0];
    check(std::fabs(e.gps_time - (2400 * 604800.0 + 1.0)) < 1e-9, "stamped with GSOF 1's time, whatever the order");
    check(e.position && std::fabs(e.position->lat - 49.1 * kDeg) < 1e-12, "position in radians");
    check(e.velocity && std::fabs(e.velocity->v_ned.x()) < 1e-9 && std::fabs(e.velocity->v_ned.y() - 20.0) < 1e-6 &&
              std::fabs(e.velocity->v_ned.z() + 0.5) < 1e-9,
          "heading 90 is east; vertical up is negative down");
    check(e.attitude && std::fabs(e.attitude->yaw - 91.0 * kDeg) < 1e-12, "attitude in radians");
    check(!e.attitude->cov.has_value(), "short-form attitude carries no covariance");
}

void testConsecutive()
{
    // Two transmissions 100 ms apart: two epochs, each with its own time.
    GnssAssembler a;
    transmission(a, 100.0, 1000);
    transmission(a, 100.1, 1100);
    const auto epochs = a.poll(100.2);
    check(epochs.size() == 2, "two transmissions, two epochs");
    if (epochs.size() == 2) check(epochs[1].gps_time - epochs[0].gps_time > 0.099, "each keeps its own time");
    // At 50 Hz they are 20 ms apart: still separate, because a second
    // position closes the first burst.
    GnssAssembler fast;
    transmission(fast, 200.0, 5000);
    transmission(fast, 200.012, 5020);  // inside the window, but a repeat
    check(fast.poll(201.0).size() == 2, "a repeated record type closes the burst early");
}

void testNoTime()
{
    // A receiver not sending GSOF 1: position alone has no time, and must
    // not be given one.
    GnssAssembler a;
    a.addPosition({49.1, 8.5, 110.0}, 10.0);
    a.addVelocity({true, 5.0, 0.0, 0.0}, 10.001);
    check(a.poll(11.0).empty(), "a position with nothing to date it is dropped");
    check(a.stats().noTime == 1, "and counted");

    // Attitude alone is dated by its own time of week, once a week is known.
    AttitudeRecord att;
    att.timeOfWeekMs = 7000;
    att.pitchValid = att.yawValid = true;
    a.addAttitude(att, 12.0);
    check(a.poll(13.0).empty() && a.stats().noWeek == 1, "no week yet: dropped and counted");
    a.addWeek(2401);
    a.addAttitude(att, 14.0);
    const auto epochs = a.poll(15.0);
    check(epochs.size() == 1 && epochs[0].attitude &&
              std::fabs(epochs[0].gps_time - (2401 * 604800.0 + 7.0)) < 1e-9,
          "attitude-only epoch dated by GSOF 27's own time");
}

void testSlowRecords()
{
    GnssAssembler a;
    a.addSigma({0.03, 0.02, 0.0001, 0.05}, 99.5);
    a.addFix(vehicle_estimator::FixQuality::rtx, 99.5);
    transmission(a, 100.0, 1000);
    auto e = a.poll(100.1);
    check(e.size() == 1 && e[0].fix == vehicle_estimator::FixQuality::rtx, "a fresh fix type is used");
    check(e.size() == 1 && std::fabs(e[0].position->cov_ned(0, 0) - 0.0004) < 1e-12 &&
              std::fabs(e[0].position->cov_ned(1, 1) - 0.0009) < 1e-12 &&
              std::fabs(e[0].position->cov_ned(0, 1) - 0.0001) < 1e-12,
          "a fresh sigma: north first, east second, the cross term kept");
    // Ten seconds later neither is fresh: the defaults, not stale values.
    transmission(a, 110.0, 11000);
    e = a.poll(110.1);
    check(e.size() == 1 && e[0].fix == vehicle_estimator::FixQuality::autonomous, "a stale fix type is not used");
    check(e.size() == 1 && std::fabs(e[0].position->cov_ned(0, 0) - 4.0) < 1e-12, "nor a stale sigma");
}

void testInvalidParts()
{
    GnssAssembler a;
    a.addPosition({49.1, 8.5, 110.0}, 1.0);
    a.addVelocity({false, 20.0, 90.0, 0.0}, 1.0);
    a.addTime({2400, 1000}, 1.0);
    AttitudeRecord att;
    att.timeOfWeekMs = 1000;
    att.yawValid = true;
    att.pitchValid = false;
    a.addAttitude(att, 1.0);
    auto e = a.poll(2.0);
    check(e.size() == 1 && !e[0].velocity, "an invalid velocity is left out");
    check(e.size() == 1 && !e[0].attitude, "an attitude without a valid pitch is left out");

    // A dual-antenna solution from a different epoch in the same burst.
    GnssAssembler b;
    b.addTime({2400, 2000}, 5.0);
    b.addPosition({49.1, 8.5, 110.0}, 5.0);
    att.pitchValid = true;
    att.timeOfWeekMs = 1900;
    b.addAttitude(att, 5.001);
    e = b.poll(6.0);
    check(e.size() == 1 && !e[0].attitude, "an attitude from another time of week is not attached");

    // Long-form variances, clamped to something a covariance can be.
    GnssAssembler c;
    c.addTime({2400, 3000}, 7.0);
    att.timeOfWeekMs = 3000;
    att.hasVariance = true;
    att.yawVariance = 1e-12;   // implausibly certain
    att.pitchVariance = 50.0;  // implausibly not
    c.addAttitude(att, 7.0);
    e = c.poll(8.0);
    check(e.size() == 1 && e[0].attitude && e[0].attitude->cov && (*e[0].attitude->cov)(0, 0) >= 1e-7 &&
              (*e[0].attitude->cov)(1, 1) <= 0.04,
          "attitude variances are clamped");
}

}  // namespace

int main()
{
    testBurst();
    testConsecutive();
    testNoTime();
    testSlowRecords();
    testInvalidParts();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("gnss assembler: all passed");
    return 0;
}
