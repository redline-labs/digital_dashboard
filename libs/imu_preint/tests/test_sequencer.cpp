// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MTi sample header, unwrapped: both counters wrap, and neither wrap may
// look like an event; a counter that skips is one, and must say how many
// samples it skipped.

#include "imu_preint/sequencer.h"

#include <spdlog/spdlog.h>

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

using Kind = imu_preint::SampleSequencer::Kind;

void testWraps()
{
    imu_preint::SampleSequencer s;
    // Start 3 samples before both counters wrap: the packet counter at
    // 65535 and SampleTimeFine 250 ticks before 2^32.
    std::uint16_t c = 65533;
    std::uint32_t t = 0xFFFFFFFFu - 250u;
    auto r = s.push(c, t);
    check(r.kind == Kind::first, "first sample");
    const double t_first = r.device_time_s;
    const std::uint64_t i_first = r.index;
    for (int k = 1; k <= 10; ++k)
    {
        ++c;
        t += 100;  // 100 Hz at 10 kHz ticks
        r = s.push(c, t);
        check(r.kind == Kind::next, fmt::format("sample {} across the wraps is just the next one", k));
        check(r.index == i_first + static_cast<std::uint64_t>(k), "index keeps counting");
        check(std::fabs(r.device_time_s - t_first - 0.01 * k) < 1e-9,
              fmt::format("device time is continuous across the tick wrap at sample {}", k));
    }
}

void testEvents()
{
    imu_preint::SampleSequencer s(10);
    s.push(100, 1000);
    auto r = s.push(101, 1100);
    check(r.kind == Kind::next, "next");

    r = s.push(101, 1100);
    check(r.kind == Kind::duplicate, "a repeated counter is a duplicate");

    r = s.push(99, 900);
    check(r.kind == Kind::out_of_order, "a counter behind the last is out of order");

    r = s.push(105, 1500);
    check(r.kind == Kind::gap && r.missing == 3, fmt::format("3 samples lost: kind gap, missing {}", r.missing));
    check(std::fabs(r.device_time_s - 0.15) < 1e-12, "time after a gap comes from the tick counter");

    r = s.push(106, 1600);
    check(r.kind == Kind::next, "the sequence continues after a gap");

    r = s.push(200, 11000);
    check(r.kind == Kind::restart && r.missing == 93, "a gap beyond the bridge limit restarts");

    // A gap that spans the counter wrap.
    imu_preint::SampleSequencer w(10);
    w.push(65534, 0);
    r = w.push(2, 400);
    check(r.kind == Kind::gap && r.missing == 3, fmt::format("gap across the wrap: missing {}", r.missing));
}

}  // namespace

int main()
{
    testWraps();
    testEvents();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("sequencer: all passed");
    return 0;
}
