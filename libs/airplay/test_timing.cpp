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

    // The limit of that trick -- and it IS reached, on every session.
    //
    // An earlier version of this test called the case "not reachable in
    // practice". Hardware said otherwise. Our clock counts from boot and the
    // phone's timestamps are in the NTP domain (seconds since 1900), which puts
    // them ~126 years apart at the first exchange. That is past the +/-2^31
    // second window a signed 64-bit fixed-point difference can represent, so
    // offsetSeconds() wraps and returns the wrong sign: a measured true gap of
    // +3.99e9 s came out as -3.05e8 s, and the 1/8 slew then took over ninety
    // seconds to crawl back to a usable clock.
    //
    // The fix is not in this function -- a difference genuinely cannot express
    // that gap -- it is that the first sample adopts the phone's clock via
    // toNanos() instead. This case is pinned so the limit stays documented.
    {
        const uint64_t ours = stamp(60000, 0.0);          // seconds since boot
        const uint64_t phone = stamp(3990000000ull, 0.0); // seconds since 1900
        const double offset = offsetSeconds(ours, phone, phone, ours);
        expect(offset < 0.0,
               "a real boot-clock vs NTP-clock gap wraps to the WRONG SIGN -- which is why "
               "the first sample adopts rather than steps");
    }

    // toNanos is what the adoption uses, and it has to stay exact across the
    // whole NTP range: an accessory that adopts a wrong clock is worse off than
    // one that never synced.
    {
        expect(toNanos(stamp(0, 0.0)) == 0, "the epoch is zero nanoseconds");
        expect(toNanos(stamp(1, 0.0)) == 1000000000LL, "one second");
        expect(toNanos(stamp(0, 0.5)) == 500000000LL, "half a second of fraction");

        // The real magnitude, which must not overflow int64 (4.29e9 s is
        // 4.29e18 ns against a 9.22e18 ceiling).
        const int64_t big = toNanos(stamp(4290000000ull, 0.0));
        expect(big == 4290000000LL * 1000000000LL, "the top of the NTP second range is exact");
        expect(big > 0, "and does not overflow to negative");

        // Round trip against offsetSeconds for a small, in-window difference:
        // the two must agree about what a second is.
        const uint64_t a = stamp(1000, 0.0);
        const uint64_t b = stamp(1002, 0.0);
        expectNear(static_cast<double>(toNanos(b) - toNanos(a)) / 1e9,
                   offsetSeconds(a, b, b, a), 1e-6,
                   "toNanos and offsetSeconds agree on an in-window difference");
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
