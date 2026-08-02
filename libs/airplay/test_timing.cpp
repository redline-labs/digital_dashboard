// SPDX-License-Identifier: GPL-3.0-or-later
//
// NTP timestamps and the clock offset computed from them.
//
// Clock sync is mandatory: without it the phone tears the session down a few
// seconds after RECORD, which presents as an unstable link rather than as a
// clock problem. The arithmetic is the part that can be wrong, and the case
// that breaks a naive version is the first exchange of a session -- the phone's
// clock runs on its own base, so the difference between its timestamps and ours
// is arbitrarily large and may go either way.
#include "airplay/timing.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

void expectNear(double got, double want, double tolerance, const std::string& what)
{
    if (std::abs(got - want) > tolerance)
    {
        SPDLOG_ERROR("FAIL: {} (got {}, want {})", what, got, want);
        ++failures;
    }
}

// An NTP timestamp from whole seconds plus a fraction.
uint64_t stamp(uint64_t seconds, double fraction = 0.0)
{
    constexpr double kTwo32 = 4294967296.0;
    return (seconds << 32) | static_cast<uint64_t>(fraction * kTwo32);
}

}  // namespace

int main()
{
    using namespace airplay::ntp;

    // The wire format: seconds in the high 32 bits, fraction in the low, big
    // endian. Getting the byte order wrong yields offsets of millions of
    // seconds, which the step path would happily apply.
    {
        uint8_t buffer[8] = {};
        write(buffer, 0x0102030405060708ull);
        expect(buffer[0] == 0x01 && buffer[1] == 0x02 && buffer[2] == 0x03 && buffer[3] == 0x04,
               "seconds are written big endian");
        expect(buffer[4] == 0x05 && buffer[5] == 0x06 && buffer[6] == 0x07 && buffer[7] == 0x08,
               "the fraction follows, big endian");
        expect(read(buffer) == 0x0102030405060708ull, "and reads back");

        uint8_t zero[8] = {};
        expect(read(zero) == 0, "zero round trips");

        uint8_t max[8];
        write(max, 0xFFFFFFFFFFFFFFFFull);
        expect(read(max) == 0xFFFFFFFFFFFFFFFFull, "the full range round trips");
    }

    // Two clocks already in step, with symmetric delay: no offset.
    {
        const uint64_t t1 = stamp(1000, 0.0);
        const uint64_t t2 = stamp(1000, 0.01);   // 10 ms of flight
        const uint64_t t3 = stamp(1000, 0.02);   // 10 ms to turn around
        const uint64_t t4 = stamp(1000, 0.03);
        expectNear(offsetSeconds(t1, t2, t3, t4), 0.0, 1e-6,
                   "matched clocks with symmetric delay have no offset");
    }

    // Our clock is a second behind the phone's. The offset is what we add.
    {
        const uint64_t t1 = stamp(1000, 0.0);
        const uint64_t t2 = stamp(1001, 0.01);
        const uint64_t t3 = stamp(1001, 0.02);
        const uint64_t t4 = stamp(1000, 0.03);
        expectNear(offsetSeconds(t1, t2, t3, t4), 1.0, 1e-6, "a clock one second behind");
    }

    // And ahead: the sign has to come out negative.
    {
        const uint64_t t1 = stamp(1000, 0.0);
        const uint64_t t2 = stamp(999, 0.01);
        const uint64_t t3 = stamp(999, 0.02);
        const uint64_t t4 = stamp(1000, 0.03);
        expectNear(offsetSeconds(t1, t2, t3, t4), -1.0, 1e-6, "a clock one second ahead");
    }

    // The first exchange of a session: the phone's clock base is unrelated to
    // ours and a long way off. Computing the differences as unsigned and
    // reinterpreting them as signed is what makes this come out right -- doing
    // it in unsigned gives a vast positive number whichever way the clocks lie.
    {
        const uint64_t ours = stamp(100, 0.0);
        const uint64_t theirs = stamp(1000000000ull, 0.0);  // ~31 years apart

        expectNear(offsetSeconds(ours, theirs, theirs, ours), 1000000000.0 - 100.0, 1.0,
                   "a clock base decades away yields the true difference");
        expectNear(offsetSeconds(theirs, ours, ours, theirs), -(1000000000.0 - 100.0), 1.0,
                   "and a base decades behind yields a negative offset");
    }

    // The limit of that trick, which is worth knowing rather than discovering:
    // the signed reinterpretation is over a 64-bit fixed-point value, so it
    // holds while the two clocks are within +/-2^31 seconds -- about 68 years,
    // the same bound NTP itself has. Past it the difference wraps and comes out
    // with the wrong sign.
    //
    // This is not reachable in practice (the phone would have to be ~95 years
    // from us) and is asserted only so a future change to the arithmetic that
    // silently narrows the range shows up here.
    {
        const uint64_t ours = stamp(100, 0.0);
        const uint64_t beyond = stamp(3000000000ull, 0.0);  // ~95 years apart
        const double offset = offsetSeconds(ours, beyond, beyond, ours);
        expect(offset < 0.0, "past 68 years apart the difference wraps and the sign inverts");
    }

    // Across the 2^32-second rollover. The unsigned difference wraps to the
    // right small value; a signed comparison of the raw stamps would not.
    {
        const uint64_t before = stamp(0xFFFFFFFFull, 0.0);
        const uint64_t after = stamp(0x00000001ull, 0.0);  // two seconds later, wrapped
        expectNear(offsetSeconds(before, after, after, before), 2.0, 1e-6,
                   "a timestamp wrapping past 2^32 seconds still reads as two seconds on");
    }

    // Asymmetric delay biases the offset by half the asymmetry -- inherent to
    // the algorithm, and worth stating so it is not mistaken for a bug.
    {
        const uint64_t t1 = stamp(1000, 0.0);
        const uint64_t t2 = stamp(1000, 0.05);   // 50 ms out
        const uint64_t t3 = stamp(1000, 0.06);
        const uint64_t t4 = stamp(1000, 0.07);   // 10 ms back
        expectNear(offsetSeconds(t1, t2, t3, t4), 0.02, 1e-6,
                   "asymmetric delay biases the offset by half the difference");
    }

    // Sub-millisecond resolution survives the fixed-point conversion.
    {
        const uint64_t t1 = stamp(1000, 0.0);
        const uint64_t t2 = stamp(1000, 0.0001);
        const uint64_t t3 = stamp(1000, 0.0001);
        const uint64_t t4 = stamp(1000, 0.0002);
        expectNear(offsetSeconds(t1, t2, t3, t4), 0.0, 1e-6, "tenths of a millisecond resolve");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("timing tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
