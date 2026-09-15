// SPDX-License-Identifier: GPL-3.0-or-later
// Widget-side touch throttling: the interval boundary, the deferral state
// machine, and the gesture transitions. Time is injected, so unlike
// carplay_test_touch_rate (which drives a real widget over a real second of
// wall clock) every case here is exact. No Qt, no zenoh, no threads.
#include "carplay/touch_throttle.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace
{

using Action = TouchThrottle::Action;
using Point = TouchThrottle::Point;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

// 60 Hz, matching the widget.
constexpr auto kInterval = std::chrono::microseconds(1'000'000 / 60);

const TouchThrottle::clock::time_point kT0{};

TouchThrottle::clock::time_point at(std::chrono::microseconds offset)
{
    return kT0 + offset;
}

TouchThrottle::clock::time_point atUs(int64_t us)
{
    return at(std::chrono::microseconds(us));
}

void testFirstMoveOfADragIsImmediate()
{
    // Leading edge: a drag must start responding at once, not up to an interval
    // later. Asserted at the clock epoch on purpose -- that is the value that
    // catches a "last sent" timestamp left at its default.
    TouchThrottle t(kInterval);
    expect(t.onMove({1, 1}, kT0).action == Action::Publish,
           "the first move of a drag publishes immediately");
    expect(!t.hasPending(), "and leaves nothing pending");
}

void testIntervalBoundaryIsExact()
{
    TouchThrottle t(kInterval);
    t.onMove({0, 0}, kT0);  // consume the leading edge

    const int64_t interval_us = kInterval.count();

    expect(t.onMove({1, 1}, atUs(interval_us - 1)).action == Action::Defer,
           "one microsecond before the interval, motion defers");

    TouchThrottle t2(kInterval);
    t2.onMove({0, 0}, kT0);
    expect(t2.onMove({1, 1}, atUs(interval_us)).action == Action::Publish,
           "exactly on the interval, motion publishes");

    TouchThrottle t3(kInterval);
    t3.onMove({0, 0}, kT0);
    expect(t3.onMove({1, 1}, atUs(interval_us + 1)).action == Action::Publish,
           "one microsecond after the interval, motion publishes");

    // Pinned at the resolution of the clock itself, not just to the microsecond
    // the rest of this file works in -- otherwise a boundary that is off by a
    // nanosecond or two slips through every case above.
    TouchThrottle t4(kInterval);
    t4.onMove({0, 0}, kT0);
    const auto one_ns_early = kT0 + kInterval - std::chrono::nanoseconds(1);
    expect(t4.onMove({1, 1}, one_ns_early).action == Action::Defer,
           "one nanosecond before the interval, motion still defers");
}

void testDeferReportsTimeRemaining()
{
    TouchThrottle t(kInterval);
    t.onMove({0, 0}, kT0);

    const auto quarter = kInterval / 4;
    const auto decision = t.onMove({1, 1}, at(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 quarter)));
    expect(decision.action == Action::Defer, "motion a quarter of the way in defers");
    expect(decision.wait == kInterval - quarter,
           "and reports exactly the remaining time, so the caller arms one timer");
}

void testDeferredPositionIsOverwrittenByNewest()
{
    // The coalescing rule: stale samples must never accumulate or win.
    TouchThrottle t(kInterval);
    t.onMove({0, 0}, kT0);

    t.onMove({1, 1}, atUs(1));
    t.onMove({2, 2}, atUs(2));
    t.onMove({9, 9}, atUs(3));
    expect(t.hasPending(), "motion inside the interval is held");

    const auto flushed = t.takePending(at(kInterval));
    expect(flushed.has_value(), "the flush yields a position");
    expect(*flushed == Point{9, 9}, "which is the newest, not the first deferred");
    expect(!t.hasPending(), "and nothing is left pending");
}

void testFlushRearmsTheInterval()
{
    // A flush is a send, so the next move is spaced against it -- otherwise a
    // steady drag would publish at twice the intended rate.
    TouchThrottle t(kInterval);
    t.onMove({0, 0}, kT0);
    t.onMove({1, 1}, atUs(1));
    t.takePending(at(kInterval));

    expect(t.onMove({2, 2}, atUs(kInterval.count() + 1)).action == Action::Defer,
           "motion right after a flush defers against the flush, not the older send");
}

void testFlushWithNothingPendingIsHarmless()
{
    // The timer can outlive what it was armed for -- a publish or a transition
    // may have consumed the pending position first.
    TouchThrottle t(kInterval);
    expect(!t.takePending(kT0).has_value(), "a flush with nothing pending yields nothing");

    t.onMove({1, 1}, kT0);  // publishes, nothing pending
    expect(!t.takePending(at(kInterval)).has_value(),
           "a flush after the pending position was already published yields nothing");
}

void testDownDropsMotionFromThePreviousGesture()
{
    // Landing a stale move after a press would report the finger somewhere it
    // no longer is.
    TouchThrottle t(kInterval);
    t.onMove({0, 0}, kT0);
    t.onMove({5, 5}, atUs(1));
    expect(t.hasPending(), "motion is pending");

    t.onDown(atUs(2));
    expect(!t.hasPending(), "a press drops it");
    expect(!t.takePending(at(kInterval)).has_value(),
           "and a flush that fires afterwards publishes nothing");
}

void testDownSpacesTheFollowingMotion()
{
    // The press already carried a position, so motion immediately after it
    // should not double up on the wire.
    TouchThrottle t(kInterval);
    t.onDown(kT0);
    expect(t.onMove({1, 1}, atUs(1)).action == Action::Defer,
           "motion right after a press is spaced against it");
}

void testUpFlushesPendingMotionThatDiffers()
{
    // Keeps the tail of the gesture dense, which is what the phone measures to
    // decide fling velocity.
    TouchThrottle t(kInterval);
    t.onMove({0, 0}, kT0);
    t.onMove({5, 5}, atUs(1));

    const auto flush = t.onUp({7, 7}, atUs(2));
    expect(flush.has_value(), "a release flushes motion still pending");
    expect(*flush == Point{5, 5}, "at the pending position, to be sent before the release");
    expect(!t.hasPending(), "and clears it");
}

void testUpSuppressesADuplicateFlush()
{
    // The release carries its own coordinate; publishing the same point twice
    // would be pure noise on a link we are trying not to flood.
    TouchThrottle t(kInterval);
    t.onMove({0, 0}, kT0);
    t.onMove({5, 5}, atUs(1));

    expect(!t.onUp({5, 5}, atUs(2)).has_value(),
           "a release at the pending position does not flush a duplicate");
}

void testUpWithNothingPendingFlushesNothing()
{
    TouchThrottle t(kInterval);
    t.onMove({3, 3}, kT0);  // published, not pending
    expect(!t.onUp({3, 3}, atUs(1)).has_value(), "a release with nothing pending flushes nothing");

    TouchThrottle t2(kInterval);
    expect(!t2.onUp({1, 1}, kT0).has_value(), "a tap with no motion at all flushes nothing");
}

void testTapsAreNeverRateLimited()
{
    // Down and up are user-initiated state transitions; delaying them is felt
    // directly. They have no gate at all -- onDown/onUp always proceed.
    TouchThrottle t(kInterval);
    for (int i = 0; i < 10; ++i)
    {
        // All ten taps inside a single interval.
        t.onDown(atUs(i * 2));
        expect(!t.onUp({1, 1}, atUs(i * 2 + 1)).has_value(),
               "a tap inside the interval still completes without deferral");
    }
}

void testSteadyDragConvergesOnTheIntervalRate()
{
    // The whole point, stated end to end: motion arriving far faster than the
    // interval comes out at the interval, and always at the newest position.
    TouchThrottle t(kInterval);
    const int64_t interval_us = kInterval.count();

    int published = 0;
    int64_t previous_us = -1;
    int64_t min_gap_us = INT64_MAX;
    // 10 intervals' worth of wall time, sampled every 200 us (~5 kHz in).
    const int64_t span_us = interval_us * 10;
    for (int64_t us = 0; us <= span_us; us += 200)
    {
        if (t.onMove({static_cast<double>(us), 0}, atUs(us)).action == Action::Publish)
        {
            ++published;
            if (previous_us >= 0)
            {
                min_gap_us = std::min(min_gap_us, us - previous_us);
            }
            previous_us = us;
        }
    }

    // The invariant, not the arithmetic: no two sends are closer than the
    // interval. The exact count depends on how the 200 us sampling lands
    // against the boundary, so it is bounded rather than pinned.
    expect(min_gap_us >= interval_us,
           "no two publishes are closer together than the interval");
    expect(published >= 10 && published <= 11,
           "a 5 kHz drive publishes about once per interval over 10 intervals (got " +
               std::to_string(published) + ")");
}

}  // namespace

int main()
{
    testFirstMoveOfADragIsImmediate();
    testIntervalBoundaryIsExact();
    testDeferReportsTimeRemaining();
    testDeferredPositionIsOverwrittenByNewest();
    testFlushRearmsTheInterval();
    testFlushWithNothingPendingIsHarmless();
    testDownDropsMotionFromThePreviousGesture();
    testDownSpacesTheFollowingMotion();
    testUpFlushesPendingMotionThatDiffers();
    testUpSuppressesADuplicateFlush();
    testUpWithNothingPendingFlushesNothing();
    testTapsAreNeverRateLimited();
    testSteadyDragConvergesOnTheIntervalRate();

    if (failures == 0)
    {
        SPDLOG_INFO("all touch throttle tests passed");
    }
    return failures == 0 ? 0 : 1;
}
