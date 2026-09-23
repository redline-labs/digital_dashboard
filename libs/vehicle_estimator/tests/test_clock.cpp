// SPDX-License-Identifier: GPL-3.0-or-later
//
// Time alignment from arrival times: the minimum-latency envelope, its
// window, the settle period, and the slew limit that keeps IMU stamps
// monotonic when the envelope moves.

#include "vehicle_estimator/clock.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <limits>
#include <random>
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

void testEnvelope()
{
    vehicle_estimator::ClockOffset c(10.0);
    check(!c.offset().has_value(), "nothing observed, no offset");
    c.observe(0.0, 100.05);
    c.observe(1.0, 101.02);  // the fastest arrival: offset 100.02
    c.observe(2.0, 102.09);
    check(std::fabs(*c.offset() - 100.02) < 1e-12, "the minimum of host - device");
    // It survives until it leaves the window, then the next minimum takes over.
    c.observe(10.5, 110.54);
    check(std::fabs(*c.offset() - 100.02) < 1e-12, "still inside the window");
    c.observe(11.5, 111.56);
    check(std::fabs(*c.offset() - 100.04) < 1e-12, "the old minimum expired");
    // Garbage does not move it.
    c.observe(std::numeric_limits<double>::quiet_NaN(), 0.0);
    c.observe(12.0, std::numeric_limits<double>::infinity());
    check(std::fabs(*c.offset() - 100.04) < 1e-12, "NaN and inf are ignored");
}

void testMapping()
{
    // Two streams on unrelated clocks, each arriving with a latency of a floor
    // plus exponential jitter. With equal floors the mapping should put IMU
    // samples on GPS time to within a fraction of a millisecond.
    std::mt19937 rng(3);
    std::exponential_distribution<double> jitter(1.0 / 0.004);
    vehicle_estimator::TimeMapper m;
    const double imu_clock = 1234.5, gps_clock = 1.47e9, host_clock = 1.79e9;
    double worst = 0.0, last = -1.0;
    bool monotonic = true;
    std::size_t mapped = 0;
    for (int k = 0; k < 3000; ++k)
    {
        const double t = 0.01 * k;
        m.observeImu(imu_clock + t, host_clock + t + 0.003 + jitter(rng));
        if (k % 10 == 0) m.observeGnss(gps_clock + t, host_clock + t + 0.003 + jitter(rng));
        const auto g = m.imuToGps(imu_clock + t);
        if (!g) continue;
        ++mapped;
        if (t > 5.0) worst = std::max(worst, std::fabs(*g - (gps_clock + t)));
        if (*g <= last) monotonic = false;
        last = *g;
    }
    SPDLOG_INFO("mapping: worst error after 5 s {:.2f} ms", worst * 1e3);
    check(mapped > 2700, "mapping available after the settle period");
    check(worst < 0.002, "mapped to within 2 ms");
    check(monotonic, "mapped times never go backwards");
}

void testSettleAndSlew()
{
    vehicle_estimator::TimeMapper m(20.0, 2.0, 1e-3);
    m.observeImu(0.0, 100.0);
    m.observeGnss(500.0, 100.0);
    check(!m.imuToGps(0.0).has_value(), "no mapping before the settle period");
    for (int k = 1; k <= 300; ++k)
    {
        const double t = 0.01 * k;
        m.observeImu(t, 100.0 + t);
        if (k % 10 == 0) m.observeGnss(500.0 + t, 100.0 + t);
    }
    const auto before = m.imuToGps(3.0);
    check(before && std::fabs(*before - 503.0) < 1e-9, "settled mapping");
    // A sample that arrives 50 ms early -- the envelope jumps -- moves the
    // mapping by at most the slew over the elapsed time, not by 50 ms.
    m.observeImu(3.01, 100.0 + 3.01 - 0.05);
    const auto after = m.imuToGps(3.01);
    check(after && std::fabs(*after - 503.01) <= 0.01 * 1e-3 + 1e-9,
          fmt::format("slew-limited: moved {:.3g} s", after ? *after - 503.01 : -1.0));
}

}  // namespace

int main()
{
    testEnvelope();
    testMapping();
    testSettleAndSlew();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("clock: all passed");
    return 0;
}
