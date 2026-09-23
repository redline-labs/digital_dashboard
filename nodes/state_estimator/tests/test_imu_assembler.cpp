// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MTi's delta_q and delta_v, published on two topics, joined back into
// one sample by packet counter -- in either order, across the counter's wrap,
// and without waiting forever for a half that never comes.

#include "imu_assembler.h"

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

using state_estimator::ImuAssembler;
using state_estimator::ImuHeader;

Eigen::Quaterniond q(double x)
{
    return Eigen::Quaterniond(Eigen::AngleAxisd(x, Eigen::Vector3d::UnitZ()));
}

void testPairing()
{
    ImuAssembler a;
    std::uint16_t c = 65530;  // across the wrap
    std::uint32_t tick = 1000;
    for (int k = 0; k < 20; ++k, ++c, tick += 100)
    {
        const ImuHeader h{c, tick};
        // Alternate which half lands first.
        if (k % 2)
        {
            a.addDeltaQ(h, q(0.001 * k), 1.0 + 0.01 * k);
            a.addDeltaV(h, Eigen::Vector3d(0, 0, 0.098 + k), 1.0 + 0.01 * k + 0.0001);
        }
        else
        {
            a.addDeltaV(h, Eigen::Vector3d(0, 0, 0.098 + k), 1.0 + 0.01 * k);
            a.addDeltaQ(h, q(0.001 * k), 1.0 + 0.01 * k + 0.0001);
        }
    }
    const auto out = a.take();
    check(out.size() == 20, fmt::format("twenty samples paired, got {}", out.size()));
    bool ordered = true, matched = true;
    for (std::size_t k = 0; k < out.size(); ++k)
    {
        if (out[k].packet_counter != static_cast<std::uint16_t>(65530 + k)) ordered = false;
        if (std::fabs(out[k].dv.z() - (0.098 + static_cast<double>(k))) > 1e-12) matched = false;
    }
    check(ordered, "in counter order, with the device's own counters across the wrap");
    check(matched, "each dq with its own dv");
}

void testLateHalf()
{
    // The dv of sample 5 arrives after sample 6 is complete: 5 is still
    // emitted, and in order.
    ImuAssembler a;
    a.addDeltaQ({5, 500}, q(0.0), 1.0);
    a.addDeltaQ({6, 600}, q(0.0), 1.01);
    a.addDeltaV({6, 600}, Eigen::Vector3d::Zero(), 1.011);
    check(a.take().empty(), "nothing emitted past an incomplete earlier sample");
    a.addDeltaV({5, 500}, Eigen::Vector3d::Zero(), 1.012);
    const auto out = a.take();
    check(out.size() == 2 && out[0].packet_counter == 5 && out[1].packet_counter == 6, "then both, in order");
}

void testOrphan()
{
    // A dq whose dv never comes is abandoned once the stream is well past it.
    ImuAssembler a(5);
    a.addDeltaQ({100, 0}, q(0.0), 1.0);
    for (std::uint16_t c = 101; c <= 110; ++c)
    {
        a.addDeltaQ({c, 0}, q(0.0), 1.0);
        a.addDeltaV({c, 0}, Eigen::Vector3d::Zero(), 1.0);
    }
    const auto out = a.take();
    check(a.orphans() == 1, "the lonely half is counted");
    check(out.size() == 10 && out.front().packet_counter == 101, "and the stream behind it flows");
}

}  // namespace

int main()
{
    testPairing();
    testLateHalf();
    testOrphan();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("imu assembler: all passed");
    return 0;
}
