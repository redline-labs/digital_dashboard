// SPDX-License-Identifier: GPL-3.0-or-later
//
// NTP64 <-> nanoseconds.
//
// Worth testing carefully for one reason: every way of getting this wrong
// produces a number rather than an error. An NTP64 read as nanoseconds, or as
// seconds, or with the halves swapped, is still a uint64 that a bag file will
// store, a latency figure will print, and a plot will put on an axis. Nothing
// downstream can tell it apart from a correct answer -- the only place the
// mistake is visible is here.
//
// The specific traps, each of which has its own case below:
//
//   - reading the whole word as nanoseconds (off by ~2^32);
//   - dividing the fraction by 2^32 in integers before scaling to nanoseconds,
//     which truncates every fraction under a second to zero;
//   - scaling the fraction in double, which loses the low nanoseconds;
//   - overflowing while scaling: fraction * 1e9 needs 62 bits.

#include "pub_sub/timestamp.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void expectEqual(std::uint64_t actual, std::uint64_t expected, const std::string& what)
{
    ++checks;
    if (actual != expected)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s (got %llu, expected %llu)\n", what.c_str(),
                     static_cast<unsigned long long>(actual),
                     static_cast<unsigned long long>(expected));
    }
}

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ull;

// A whole number of seconds has an all-zero fraction, so this is the case where
// the two halves cannot be confused with each other.
void testWholeSeconds()
{
    expectEqual(pub_sub::ntp64ToUnixNanos(0u), 0u, "the epoch is zero");

    expectEqual(pub_sub::ntp64ToUnixNanos(1ull << 32u), kNanosPerSecond,
                "one second is 1e9 nanoseconds");

    // 2026-08-05T00:00:00Z. A real-looking value, because the failure this
    // guards is a plausible wrong magnitude rather than nonsense.
    constexpr std::uint64_t kSeconds = 1'785'888'000ull;
    expectEqual(pub_sub::ntp64ToUnixNanos(kSeconds << 32u), kSeconds * kNanosPerSecond,
                "a 2026 timestamp converts to the matching nanosecond count");
}

// The fraction is 2^-32 of a second, NOT nanoseconds and NOT milliseconds.
void testFractions()
{
    // Half a second is the top bit of the fraction set.
    expectEqual(pub_sub::ntp64ToUnixNanos(1ull << 31u), 500'000'000ull,
                "the top fraction bit is half a second");

    expectEqual(pub_sub::ntp64ToUnixNanos(1ull << 30u), 250'000'000ull,
                "the next fraction bit is a quarter second");

    // An all-ones fraction is one tick short of a full second. Truncating
    // division means this lands just under 1e9, never at or above it -- if it
    // ever reached 1e9 the seconds field would effectively double-count.
    const std::uint64_t almost = pub_sub::ntp64ToUnixNanos(0xFFFF'FFFFull);
    expect(almost < kNanosPerSecond, "an all-ones fraction stays under one second");
    expect(almost > kNanosPerSecond - 10u, "an all-ones fraction is within 10ns of one second");

    // The trap: (fraction / 2^32) * 1e9 in integer arithmetic is zero for every
    // fraction below a full second. If the implementation did that, every one of
    // these would come back as exactly the seconds boundary.
    expect(pub_sub::ntp64ToUnixNanos((1ull << 32u) | (1ull << 31u)) ==
               kNanosPerSecond + 500'000'000ull,
           "seconds and fraction combine rather than the fraction being dropped");
}

// The scaling multiplies the fraction by 1e9 before shifting. That intermediate
// needs 62 bits; doing it in 32 bits, or shifting first, is wrong.
void testNoOverflowAtTheTop()
{
    // Largest representable NTP64: all ones. Seconds = 2^32-1, which is the
    // 2106 ceiling of the format itself (zenoh's own HLC hits its 2036 limit
    // sooner -- see the header).
    const std::uint64_t max = pub_sub::ntp64ToUnixNanos(0xFFFF'FFFF'FFFF'FFFFull);
    const std::uint64_t expected_seconds = 0xFFFF'FFFFull;

    expect(max / kNanosPerSecond == expected_seconds,
           "the maximum NTP64 still yields the right second count");
    expect(max > expected_seconds * kNanosPerSecond,
           "the fraction contributes rather than overflowing to zero");
}

// Round trip. Not bit-exact in the NTP64 direction -- the fraction has ~233 ps
// of resolution, so a value that did not come from a nanosecond boundary moves.
// Through nanoseconds it must be stable.
void testRoundTrip()
{
    // Spelled out, because `unsigned long long` and `std::uint64_t` are
    // distinct types where the latter is `unsigned long`, and a braced list
    // holding both deduces to neither.
    for (const std::uint64_t nanos : std::initializer_list<std::uint64_t> {
             0,
             1,
             999'999'999,
             kNanosPerSecond,
             1'785'888'000 * kNanosPerSecond,
             1'785'888'000 * kNanosPerSecond + 123'456'789 })
    {
        const std::uint64_t there_and_back =
            pub_sub::ntp64ToUnixNanos(pub_sub::unixNanosToNtp64(nanos));

        // One nanosecond of slack: 1e9 does not divide 2^32, so not every
        // nanosecond value is exactly representable as a fraction.
        const std::uint64_t difference =
            there_and_back > nanos ? there_and_back - nanos : nanos - there_and_back;
        expect(difference <= 1u,
               "nanos -> NTP64 -> nanos is stable to within a nanosecond for " +
                   std::to_string(nanos));
    }
}

// Ordering has to survive the conversion, because that is what a bag's index and
// a latency measurement actually rely on -- not the absolute value.
void testMonotonic()
{
    std::uint64_t previous = 0u;
    bool ordered = true;

    for (std::uint64_t ntp = 0u; ntp < (1ull << 34u); ntp += (1ull << 26u))
    {
        const std::uint64_t nanos = pub_sub::ntp64ToUnixNanos(ntp);
        if (ntp != 0u && nanos <= previous)
        {
            ordered = false;
        }
        previous = nanos;
    }

    expect(ordered, "conversion is strictly increasing across a seconds boundary");
}

// The bottom bits are uhlc's logical counter, not time. Two stamps differing
// only there are the same instant as far as anything here is concerned; the
// point of this case is that such a difference does NOT vanish into an equal
// nanosecond value by accident -- it may or may not, and code must not depend
// on either. What must hold is that it never goes backwards.
void testLogicalCounterBitsDoNotInvertOrder()
{
    const std::uint64_t base = (1'785'888'000ull << 32u);
    expect(pub_sub::ntp64ToUnixNanos(base + 1u) >= pub_sub::ntp64ToUnixNanos(base),
           "a logical-counter increment never moves time backwards");
    expect(pub_sub::ntp64ToUnixNanos(base + 15u) >= pub_sub::ntp64ToUnixNanos(base),
           "a full 4-bit counter never moves time backwards");
    expect(pub_sub::ntp64ToUnixNanos(base + 15u) - pub_sub::ntp64ToUnixNanos(base) < 10u,
           "the 4 counter bits are worth under 10 nanoseconds");
}

// constexpr, so a caller can use it in a constant expression and so the compiler
// checks the arithmetic at build time.
static_assert(pub_sub::ntp64ToUnixNanos(1ull << 32u) == kNanosPerSecond);
static_assert(pub_sub::ntp64ToUnixNanos(1ull << 31u) == 500'000'000ull);
static_assert(pub_sub::unixNanosToNtp64(kNanosPerSecond) == (1ull << 32u));

}  // namespace

int main()
{
    testWholeSeconds();
    testFractions();
    testNoOverflowAtTheTop();
    testRoundTrip();
    testMonotonic();
    testLogicalCounterBitsDoNotInvertOrder();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
