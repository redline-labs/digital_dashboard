// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every verdict rule, at its boundary. These are pure functions of time, so
// the times are made up rather than waited for.
#include "node_health/classify.h"

#include "check.h"

#include <string>

using namespace std::chrono_literals;
using node_health::Clock;
using node_health::ClassifyOptions;
using node_health::HealthSnapshot;
using node_health::Observation;
using node_health::State;
using node_health::Verdict;

namespace
{

const Clock::time_point t0 = Clock::time_point(1h);

std::string name(Verdict verdict)
{
    return std::string(node_health::to_string(verdict));
}

Observation reporting(State state, std::uint32_t period_ms, Clock::time_point received)
{
    Observation observation;
    observation.identity_known = true;
    observation.identity_reachable = true;
    observation.identity_first_seen = t0;
    HealthSnapshot snapshot;
    snapshot.state = state;
    snapshot.period_ms = period_ms;
    observation.last = snapshot;
    observation.last_received = received;
    return observation;
}

void testReportedStatesPassThrough()
{
    const ClassifyOptions options;
    test::check(classify(reporting(State::ok, 1000, t0), t0, options) == Verdict::ok, "ok is ok");
    test::check(classify(reporting(State::starting, 1000, t0), t0, options) == Verdict::starting,
                "starting is starting");
    test::check(classify(reporting(State::degraded, 1000, t0), t0, options) == Verdict::degraded,
                "degraded is degraded");
    test::check(classify(reporting(State::fault, 1000, t0), t0, options) == Verdict::fault,
                "fault is fault");
    test::check(classify(reporting(State::stopping, 1000, t0), t0, options) == Verdict::stopping,
                "stopping is stopping");
    test::check(classify(reporting(State::unknown, 1000, t0), t0, options) == Verdict::degraded,
                "a state the node could not name is degraded, not ok");
}

void testLateBoundary()
{
    const ClassifyOptions options;  // 3 periods
    const Observation observation = reporting(State::ok, 1000, t0);
    test::check(classify(observation, t0 + 3000ms, options) == Verdict::ok,
                "exactly three periods old is not yet late");
    test::check(classify(observation, t0 + 3000ms + 1ns, options) == Verdict::late,
                "one tick past three periods is late: " + name(classify(observation, t0 + 3001ms, options)));
    test::check(classify(reporting(State::fault, 1000, t0), t0 + 10s, options) == Verdict::late,
                "late outranks what a stale sample said");
}

void testPeriodClamping()
{
    const ClassifyOptions options;
    test::check(classify(reporting(State::ok, 0, t0), t0 + 3001ms, options) == Verdict::late,
                "a period of 0 counts as one second");
    test::check(classify(reporting(State::ok, 0, t0), t0 + 2999ms, options) == Verdict::ok,
                "and is not late inside three of those");
    test::check(classify(reporting(State::ok, 1, t0), t0 + 301ms, options) == Verdict::late,
                "a tiny period is clamped up to 100 ms");
    test::check(classify(reporting(State::ok, 1, t0), t0 + 299ms, options) == Verdict::ok,
                "so a sample inside 300 ms is still fresh");
    test::check(classify(reporting(State::ok, 4'000'000'000u, t0), t0 + 181s, options) == Verdict::late,
                "a huge period is clamped down to 60 s");
}

void testIdentityLoss()
{
    const ClassifyOptions options;
    Observation stopped = reporting(State::stopping, 1000, t0);
    stopped.identity_reachable = false;
    test::check(classify(stopped, t0 + 1h, options) == Verdict::exited,
                "gone after saying stopping is a clean exit, however long ago");

    Observation crashed = reporting(State::ok, 1000, t0);
    crashed.identity_reachable = false;
    test::check(classify(crashed, t0, options) == Verdict::gone, "gone without saying so is gone");

    Observation never = Observation{};
    never.identity_known = true;
    never.identity_reachable = false;
    test::check(classify(never, t0, options) == Verdict::gone, "gone with no sample at all is gone");
}

void testSilence()
{
    ClassifyOptions options;
    options.silent_grace = 3s;
    Observation observation;
    observation.identity_known = true;
    observation.identity_reachable = true;
    observation.identity_first_seen = t0;
    test::check(classify(observation, t0 + 2999ms, options) == Verdict::starting,
                "inside the grace period a quiet identity is starting");
    test::check(classify(observation, t0 + 3s, options) == Verdict::silent,
                "at the grace period it is silent");

    Observation unseen;  // a health sample without an identity, not yet arrived
    test::check(classify(unseen, t0 + 1h, options) == Verdict::starting,
                "no identity and no sample has nothing to be silent about");

    Observation health_only = reporting(State::ok, 1000, t0);
    health_only.identity_known = false;
    health_only.identity_reachable = false;
    test::check(classify(health_only, t0, options) == Verdict::ok,
                "a sample that arrives before the directory catches up is taken at its word");
}

void testHealthyVerdicts()
{
    test::check(node_health::isHealthy(Verdict::ok) && node_health::isHealthy(Verdict::exited) &&
                    node_health::isHealthy(Verdict::starting) && node_health::isHealthy(Verdict::stopping),
                "ok, starting, stopping and exited need no action");
    test::check(!node_health::isHealthy(Verdict::degraded) && !node_health::isHealthy(Verdict::fault) &&
                    !node_health::isHealthy(Verdict::late) && !node_health::isHealthy(Verdict::silent) &&
                    !node_health::isHealthy(Verdict::gone),
                "the rest do");
}

void testActivity()
{
    std::string detail;
    test::check(node_health::activityState(std::nullopt, t0, 1s, State::degraded, t0 + 999ms, detail) ==
                    State::starting,
                "before the first touch, within the window: starting");
    test::check(node_health::activityState(std::nullopt, t0, 1s, State::fault, t0 + 1s, detail) ==
                        State::fault &&
                    !detail.empty(),
                "never touched once the window has passed: the silent state, with a reason");
    test::check(node_health::activityState(t0, t0, 1s, State::degraded, t0 + 1s, detail) == State::ok &&
                    detail.empty(),
                "touched exactly the window ago is still ok");
    test::check(node_health::activityState(t0, t0, 1s, State::degraded, t0 + 1s + 1ns, detail) ==
                    State::degraded,
                "one tick later it is not");
    test::check(detail == "nothing for 1.0 s", "and says how long: '" + detail + "'");
}

void testContinuity()
{
    node_health::Continuity counters;
    HealthSnapshot first;
    first.sequence = 10;
    first.uptime_ms = 10'000;

    node_health::account(counters, nullptr, first);
    test::check(counters.restarts == 0 && counters.missed == 0, "the first sample counts nothing");

    HealthSnapshot next = first;
    next.sequence = 11;
    next.uptime_ms = 11'000;
    node_health::account(counters, &first, next);
    test::check(counters.missed == 0, "consecutive samples miss nothing");

    HealthSnapshot skipped = next;
    skipped.sequence = 15;
    skipped.uptime_ms = 15'000;
    node_health::account(counters, &next, skipped);
    test::check(counters.missed == 3, "a jump from 11 to 15 is three missed");

    HealthSnapshot restarted;
    restarted.sequence = 1;
    restarted.uptime_ms = 5;
    node_health::account(counters, &skipped, restarted);
    test::check(counters.restarts == 1 && counters.missed == 3, "sequence going backwards is a restart");

    HealthSnapshot repeated = restarted;
    node_health::account(counters, &restarted, repeated);
    test::check(counters.restarts == 2, "a repeated sequence is a restart too, not a gap");
}

}  // namespace

int main()
{
    testReportedStatesPassThrough();
    testLateBoundary();
    testPeriodClamping();
    testIdentityLoss();
    testSilence();
    testHealthyVerdicts();
    testActivity();
    testContinuity();
    return test::finish();
}
