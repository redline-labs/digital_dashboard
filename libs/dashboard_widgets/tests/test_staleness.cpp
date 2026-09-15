// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loss-of-comm state machine every bound widget runs.
//
// Pinned at the boundaries, because both ways of getting them wrong are
// invisible in a screenshot: one tick early and a gauge on a healthy stream
// flickers to NO DATA, one tick late and a dead sensor keeps its last reading
// for another delivery interval.
#include "dashboard/staleness.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace std::chrono_literals;
using dashboard::StalenessTracker;

namespace
{

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

const StalenessTracker::clock::time_point t0 = StalenessTracker::clock::time_point(1h);

void testDisabled()
{
    StalenessTracker tracker(0ms, t0);
    check(!tracker.enabled(), "a timeout of zero is disabled");
    check(tracker.poll(t0 + 10h) == StalenessTracker::Edge::none, "and never goes stale");
    check(!tracker.isStale(), "and never reports stale");
    check(tracker.onSample(t0 + 10h) == StalenessTracker::Edge::none, "and reports no edge on a sample");
}

void testNothingEverArrives()
{
    StalenessTracker tracker(500ms, t0);
    check(!tracker.isStale(), "a new binding does not start stale");
    check(tracker.poll(t0 + 499ms) == StalenessTracker::Edge::none, "nor before the timeout");
    check(tracker.poll(t0 + 500ms) == StalenessTracker::Edge::became_stale,
          "a binding that never receives anything goes stale at the timeout");
    check(tracker.isStale(), "and stays stale");
    check(tracker.poll(t0 + 10s) == StalenessTracker::Edge::none, "with the edge reported once");
}

void testBoundary()
{
    StalenessTracker tracker(500ms, t0);
    tracker.onSample(t0);
    check(tracker.poll(t0 + 500ms - 1ns) == StalenessTracker::Edge::none,
          "one tick before the timeout is still fresh");
    check(tracker.poll(t0 + 500ms) == StalenessTracker::Edge::became_stale,
          "exactly the timeout is stale");
}

void testResume()
{
    StalenessTracker tracker(200ms, t0);
    check(tracker.poll(t0 + 200ms) == StalenessTracker::Edge::became_stale, "goes stale");
    check(tracker.onSample(t0 + 250ms) == StalenessTracker::Edge::became_fresh,
          "a sample brings it back");
    check(!tracker.isStale(), "and it is fresh again");
    check(tracker.onSample(t0 + 260ms) == StalenessTracker::Edge::none,
          "with no second edge for the next sample");
    // The newest sample was at 260 ms, so the next deadline is 460 ms -- not
    // 450 ms, which is the deadline measured from the sample before it.
    check(tracker.poll(t0 + 459ms) == StalenessTracker::Edge::none,
          "and the timeout runs from the newest sample");
    check(tracker.poll(t0 + 460ms) == StalenessTracker::Edge::became_stale,
          "not from the one before it");
}

void testHugeTimeoutDoesNotOverflow()
{
    // The config clamp allows ten minutes; this is the shape of the arithmetic,
    // not the shape of a real config.
    StalenessTracker tracker(std::chrono::milliseconds(4'294'967'295), t0);
    check(tracker.enabled(), "a huge timeout is still a timeout");
    check(tracker.poll(t0 + 10h) == StalenessTracker::Edge::none, "and is not reached early");
}

void testSuppression()
{
    check(!dashboard::staleness::suppressed(), "staleness is reported by default");
    dashboard::staleness::setSuppressed(true);
    check(dashboard::staleness::suppressed(), "and can be turned off for a process");
    dashboard::staleness::setSuppressed(false);
}

}  // namespace

int main()
{
    testDisabled();
    testNothingEverArrives();
    testBoundary();
    testResume();
    testHugeTimeoutDoesNotOverflow();
    testSuppression();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
