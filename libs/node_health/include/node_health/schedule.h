// SPDX-License-Identifier: GPL-3.0-or-later
//
// When the reporter's thread next has work, as a pure function of time. It
// used to wake every 100 ms whatever the node had, which on an idle board is
// ten wakeups a second per node for nothing.
#ifndef NODE_HEALTH_SCHEDULE_H_
#define NODE_HEALTH_SCHEDULE_H_

#include "node_health/state.h"

#include <chrono>
#include <optional>
#include <span>

namespace node_health
{

// A burst of state changes becomes one sample, not one each. Also the floor on
// every interval below, so a tiny `within` cannot make the thread spin.
inline constexpr std::chrono::milliseconds kMinChangeGap{100};

// How often activity checks need evaluating: half the shortest `within`, so a
// check that goes quiet reads as such no later than 1.5x its window. nullopt
// when there are none, which leaves the thread to heartbeats and changes.
std::optional<Clock::duration> activityEvaluateInterval(std::span<const std::chrono::milliseconds> withins);

struct Schedule
{
    Clock::time_point now;
    Clock::time_point last_publish;
    std::chrono::milliseconds period{1000};
    // A state change is waiting to be published.
    bool changed = false;
    std::optional<Clock::duration> evaluate_every;
    // systemd's WatchdogSec; fed at half of it.
    std::optional<Clock::duration> watchdog;
    Clock::time_point last_watchdog;
};

// The earliest of: the next heartbeat, the change gap when a change waits, the
// next activity evaluation and the next watchdog feed. setCheck() and the like
// wake the thread early; nothing else needs to.
Clock::time_point nextWake(const Schedule& schedule);

}  // namespace node_health

#endif  // NODE_HEALTH_SCHEDULE_H_
