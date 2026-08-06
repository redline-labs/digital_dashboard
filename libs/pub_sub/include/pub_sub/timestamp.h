#ifndef PUB_SUB_TIMESTAMP_H_
#define PUB_SUB_TIMESTAMP_H_

#include <cstdint>

namespace pub_sub
{

// Zenoh sample timestamps, and what they do and do not mean.
//
// Nothing in this tree carried a publish time until sessions were opened with
// timestamping enabled (see SessionManager::buildConfig). Zenoh's default is
// `timestamping: { enabled: { router: true, peer: false, client: false } }`, and
// every session here is a peer -- so every sample arrived unstamped, and the
// only time anything had was the moment a subscriber happened to see it.
//
// THE FORMAT, and why converting it wrong is dangerous rather than merely wrong:
//
// zenoh::Timestamp::get_time() returns an NTP64 (RFC-5905): a uint64 whose upper
// 32 bits are seconds since the UNIX epoch and whose lower 32 bits are a
// fraction of a second. Reading it as "nanoseconds" or as "seconds" does not
// fail -- it produces a number that is a plausible timestamp of the wrong
// magnitude, which then flows into a bag file, a latency figure, or a plot axis
// and looks like data. So the conversion lives here, once, with a test.
//
// THREE THINGS THAT ARE NOT TRUE OF THIS CLOCK:
//
//   1. The bottom bits are not time. Zenoh stamps through a hybrid logical
//      clock (uhlc), which replaces the least significant bits of the NTP64
//      with a logical counter so that two events in the same instant still
//      order. uhlc::CSIZE is 4 bits, so the noise is under 10 ns -- irrelevant
//      for anything here, but it does mean a stamp will never compare bit-equal
//      to a system_clock reading taken at the same moment.
//
//   2. It runs out in 2036. Thirty-two bits of seconds since 1970 is the same
//      ceiling NTP has, and eclipse-zenoh/zenoh#1252 tracks it upstream. A
//      recording is a long-lived artifact, so this is written down rather than
//      discovered later.
//
//   3. It is only as good as the publisher's wall clock. The HLC re-stamps a
//      timestamp that is too far ahead of local time rather than rejecting it
//      (`drop_future_timestamp: false`), so a unit that boots with a bad RTC
//      before any time sync publishes wrong times *quietly*. This is the whole
//      reason a recorder must also record its own arrival time: a bag with a
//      bogus publish_time and a sound log_time is still usable, and one with
//      only the former is not.

// Nanoseconds since the UNIX epoch, from an NTP64 as returned by
// zenoh::Timestamp::get_time().
//
// The fraction is scaled by 1e9/2^32 in 128-bit-free arithmetic: the numerator
// (fraction * 1'000'000'000) is at most 2^32 * 10^9, which needs 62 bits, so it
// fits a uint64 with room to spare. Doing it in double instead would lose the
// low nanoseconds, and doing it as (fraction / 2^32) * 1e9 in integers would
// truncate the whole fraction to zero.
constexpr std::uint64_t ntp64ToUnixNanos(std::uint64_t ntp64)
{
    constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ull;

    const std::uint64_t seconds = ntp64 >> 32u;
    const std::uint64_t fraction = ntp64 & 0xFFFF'FFFFull;

    return seconds * kNanosPerSecond + ((fraction * kNanosPerSecond) >> 32u);
}

// The inverse, for tests and for anything synthesising a stamp (a bag replaying
// recorded times, say). Not exact in both directions: the NTP64 fraction has
// ~233 ps of resolution, so a round trip through nanoseconds is stable but a
// round trip through NTP64 may move by one nanosecond.
constexpr std::uint64_t unixNanosToNtp64(std::uint64_t nanos)
{
    constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ull;

    const std::uint64_t seconds = nanos / kNanosPerSecond;
    const std::uint64_t remainder = nanos % kNanosPerSecond;

    return (seconds << 32u) | ((remainder << 32u) / kNanosPerSecond);
}

}  // namespace pub_sub

#endif  // PUB_SUB_TIMESTAMP_H_
