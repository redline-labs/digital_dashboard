// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MTi's delta_q and delta_v, published on two topics, joined back into
// one sample by packet counter -- in either order, across the counter's wrap,
// and without waiting forever for a half that never comes. The magnetic field
// and pressure from the same packets ride along by the same counter. A sample
// is released once a later one is complete, so the newest always waits.

#include "decode.h"
#include "imu_assembler.h"
#include "xbus_environment.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

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
    // The twentieth waits for a twenty-first: the rest of its packet may
    // still be on its way.
    check(out.size() == 19, fmt::format("nineteen samples released, the newest held, got {}", out.size()));
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
    a.addDeltaQ({7, 700}, q(0.0), 1.02);
    a.addDeltaV({7, 700}, Eigen::Vector3d::Zero(), 1.021);
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
    check(out.size() == 9 && out.front().packet_counter == 101, "and the stream behind it flows");
}

void testAuxiliaryRidesAlong()
{
    // Magnetic field on every packet, pressure on every other; in any order
    // relative to the increments. Missing ones hold nothing up.
    ImuAssembler a;
    for (std::uint16_t c = 1; c <= 10; ++c)
    {
        const ImuHeader h{c, 100u * c};
        if (c != 4) a.addMagneticField(h, Eigen::Vector3d(0.1 * c, 0.2, 0.3));  // packet 4's never comes
        a.addDeltaQ(h, q(0.0), 1.0 + 0.01 * c);
        a.addDeltaV(h, Eigen::Vector3d::Zero(), 1.0 + 0.01 * c);
        if (c % 2 == 0) a.addPressure(h, 100000.0 + c);  // after its own increments
    }
    const auto out = a.take();
    check(out.size() == 9, "nine released, the tenth held for what may still come");
    bool mags = true, pressures = true;
    for (const auto& s : out)
    {
        const bool want_mag = s.packet_counter != 4;
        mags = mags && s.mag_au.has_value() == want_mag &&
               (!want_mag || std::fabs(s.mag_au->x() - 0.1 * s.packet_counter) < 1e-12);
        const bool want_p = s.packet_counter % 2 == 0;
        pressures = pressures && s.pressure_pa.has_value() == want_p &&
                    (!want_p || *s.pressure_pa == 100000.0 + s.packet_counter);
    }
    check(mags, "each magnetic field on its own packet's sample, and none where none came");
    check(pressures, "each pressure on its own, even arriving after the increments");
    check(a.orphans() == 0, "a missing magnetic field is not an orphaned increment");

    // Too late: its sample has gone.
    a.addMagneticField({3, 300}, Eigen::Vector3d::Ones());
    check(a.lateAuxiliary() == 1, "a field for a released sample is dropped and counted");
    check(a.take().empty(), "and releases nothing");
}

template <typename Schema, typename Fill>
std::vector<std::uint8_t> encode(Fill fill)
{
    ::capnp::MallocMessageBuilder b;
    fill(b.initRoot<Schema>());
    const auto words = ::capnp::messageToFlatArray(b);
    const auto bytes = words.asBytes();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

void testDecodeRefusesBadAuxiliary()
{
    state_estimator::GnssAssembler gnss;
    ImuAssembler imu;
    const auto header = [](auto h, bool clipped) {
        h.setHasPacketCounter(true);
        h.setPacketCounter(1);
        h.setHasSampleTimeFine(true);
        h.setSampleTimeFineTicks(100);
        h.setHasStatus(clipped);
        h.setClipMagnetometerY(clipped);
    };
    const auto clipped = encode<::XbusMagneticField>([&](::XbusMagneticField::Builder m) {
        header(m.initHeader(), true);
        m.setMagneticFieldXAu(3.9);
    });
    check(state_estimator::feed("XbusMagneticField", clipped, 1.0, gnss, imu) == state_estimator::Fed::ignored,
          "a clipped magnetometer sample is not used: it reads the rail, not the field");
    const auto zero = encode<::XbusBaroPressure>([&](::XbusBaroPressure::Builder p) {
        header(p.initHeader(), false);
        p.setPressurePa(0);
    });
    check(state_estimator::feed("XbusBaroPressure", zero, 1.0, gnss, imu) == state_estimator::Fed::malformed,
          "a pressure of zero is not a pressure");
    const auto good = encode<::XbusBaroPressure>([&](::XbusBaroPressure::Builder p) {
        header(p.initHeader(), false);
        p.setPressurePa(98000);
    });
    check(state_estimator::feed("XbusBaroPressure", good, 1.0, gnss, imu) == state_estimator::Fed::used,
          "a real one is used");
}

}  // namespace

int main()
{
    testPairing();
    testLateHalf();
    testOrphan();
    testAuxiliaryRidesAlong();
    testDecodeRefusesBadAuxiliary();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("imu assembler: all passed");
    return 0;
}
