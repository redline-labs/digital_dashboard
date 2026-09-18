// SPDX-License-Identifier: GPL-3.0-or-later
//
// When the reporter thread wakes. The point of the schedule is that a node
// with nothing to evaluate sleeps until its heartbeat, and that nothing it
// owes -- a change, an activity check, the systemd watchdog -- is late for it.

#include "node_health/schedule.h"

#include "check.h"

#include <array>
#include <vector>

namespace
{

using namespace node_health;
using namespace std::chrono_literals;
using test::check;

const Clock::time_point t0 = Clock::time_point(1h);

// Idle: 10 ms after a publish, 1 s heartbeat, nothing else owed.
Schedule idle()
{
    Schedule schedule;
    schedule.now = t0 + 10ms;
    schedule.last_publish = t0;
    schedule.period = 1000ms;
    return schedule;
}

void testIdleSleepsUntilTheHeartbeat()
{
    Schedule schedule = idle();
    check(nextWake(schedule) == t0 + 1000ms, "no activity checks and no change: next heartbeat");

    schedule.period = 5000ms;
    check(nextWake(schedule) == t0 + 5000ms, "a longer period sleeps longer");
}

void testAChangeWaitsOnlyForTheGap()
{
    Schedule schedule = idle();
    schedule.changed = true;
    check(nextWake(schedule) == t0 + kMinChangeGap, "a pending change goes out one gap after the last sample");
}

void testActivityChecksSetTheCadence()
{
    const std::array<std::chrono::milliseconds, 2> withins{2000ms, 1000ms};
    const auto every = activityEvaluateInterval(withins);
    check(every == std::optional<Clock::duration>(500ms), "half the shortest window");

    Schedule schedule = idle();
    schedule.evaluate_every = every;
    check(nextWake(schedule) == t0 + 510ms, "evaluated at that interval from now");

    check(!activityEvaluateInterval({}).has_value(), "no activity checks, no evaluation cadence");

    const std::array<std::chrono::milliseconds, 1> tiny{10ms};
    check(activityEvaluateInterval(tiny) == std::optional<Clock::duration>(kMinChangeGap),
          "a tiny window is floored rather than spinning the thread");

    const std::array<std::chrono::milliseconds, 1> zero{0ms};
    check(activityEvaluateInterval(zero) == std::optional<Clock::duration>(kMinChangeGap),
          "a zero window is floored too");
}

// The watchdog is fed at half its interval, so a WatchdogSec shorter than the
// heartbeat must pull the wake in -- sleeping to the heartbeat would let
// systemd kill a healthy node.
void testTheWatchdogIsNeverLate()
{
    Schedule schedule = idle();
    schedule.watchdog = 600ms;
    schedule.last_watchdog = t0;
    check(nextWake(schedule) == t0 + 300ms, "a short watchdog wakes before the heartbeat");

    schedule.watchdog = 10s;
    check(nextWake(schedule) == t0 + 1000ms, "a long one does not");

    schedule.last_watchdog = Clock::time_point{};
    check(nextWake(schedule) <= schedule.now, "a watchdog never fed is due at once");
}

void testTheEarliestWins()
{
    Schedule schedule = idle();
    schedule.now = t0 + 50ms;
    schedule.changed = true;
    schedule.evaluate_every = 500ms;
    schedule.watchdog = 400ms;
    schedule.last_watchdog = t0;
    check(nextWake(schedule) == t0 + kMinChangeGap, "the change gap is earliest here");
}

}  // namespace

int main()
{
    testIdleSleepsUntilTheHeartbeat();
    testAChangeWaitsOnlyForTheGap();
    testActivityChecksSetTheCadence();
    testTheWatchdogIsNeverLate();
    testTheEarliestWins();
    return test::finish();
}
