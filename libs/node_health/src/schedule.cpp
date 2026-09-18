// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/schedule.h"

#include <algorithm>

namespace node_health
{

std::optional<Clock::duration> activityEvaluateInterval(std::span<const std::chrono::milliseconds> withins)
{
    if (withins.empty())
    {
        return std::nullopt;
    }
    const std::chrono::milliseconds shortest = *std::min_element(withins.begin(), withins.end());
    return std::max<Clock::duration>(shortest / 2, kMinChangeGap);
}

Clock::time_point nextWake(const Schedule& schedule)
{
    Clock::time_point wake = schedule.last_publish + schedule.period;
    if (schedule.changed)
    {
        wake = std::min(wake, schedule.last_publish + kMinChangeGap);
    }
    if (schedule.evaluate_every)
    {
        wake = std::min(wake, schedule.now + *schedule.evaluate_every);
    }
    if (schedule.watchdog)
    {
        wake = std::min(wake, schedule.last_watchdog + *schedule.watchdog / 2);
    }
    return wake;
}

}  // namespace node_health
