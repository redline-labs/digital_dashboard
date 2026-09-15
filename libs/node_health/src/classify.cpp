// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/classify.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace node_health
{

namespace
{

constexpr std::chrono::milliseconds kMinPeriod{100};
constexpr std::chrono::milliseconds kMaxPeriod{60'000};
constexpr std::chrono::milliseconds kUnstatedPeriod{1000};

Verdict fromReported(State state)
{
    switch (state)
    {
        case State::ok:
            return Verdict::ok;
        case State::starting:
            return Verdict::starting;
        case State::stopping:
            return Verdict::stopping;
        case State::unknown:
        case State::degraded:
            return Verdict::degraded;
        case State::fault:
            return Verdict::fault;
    }
    return Verdict::degraded;
}

}  // namespace

std::string_view to_string(Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::ok:
            return "ok";
        case Verdict::starting:
            return "starting";
        case Verdict::degraded:
            return "degraded";
        case Verdict::fault:
            return "fault";
        case Verdict::stopping:
            return "stopping";
        case Verdict::late:
            return "late";
        case Verdict::silent:
            return "silent";
        case Verdict::exited:
            return "exited";
        case Verdict::gone:
            return "gone";
    }
    return "unknown";
}

bool isHealthy(Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::ok:
        case Verdict::starting:
        case Verdict::stopping:
        case Verdict::exited:
            return true;
        case Verdict::degraded:
        case Verdict::fault:
        case Verdict::late:
        case Verdict::silent:
        case Verdict::gone:
            return false;
    }
    return false;
}

Verdict classify(const Observation& observation, Clock::time_point now, const ClassifyOptions& options)
{
    if (observation.identity_known && !observation.identity_reachable)
    {
        const bool said_stopping = observation.last && observation.last->state == State::stopping;
        return said_stopping ? Verdict::exited : Verdict::gone;
    }

    if (!observation.last || !observation.last_received)
    {
        if (observation.identity_first_seen &&
            now - *observation.identity_first_seen >= options.silent_grace)
        {
            return Verdict::silent;
        }
        return Verdict::starting;
    }

    const std::chrono::milliseconds stated =
        observation.last->period_ms == 0 ? kUnstatedPeriod
                                         : std::chrono::milliseconds(observation.last->period_ms);
    const std::chrono::milliseconds period = std::clamp(stated, kMinPeriod, kMaxPeriod);
    const auto allowed = std::chrono::duration<double, std::milli>(period) * options.late_factor;
    if (now - *observation.last_received > allowed)
    {
        return Verdict::late;
    }

    return fromReported(observation.last->state);
}

State activityState(std::optional<Clock::time_point> last_touch, Clock::time_point created,
                    std::chrono::milliseconds within, State when_silent, Clock::time_point now,
                    std::string& detail)
{
    detail.clear();
    if (!last_touch)
    {
        if (now - created < within)
        {
            return State::starting;
        }
        detail = "nothing yet";
        return when_silent;
    }
    const auto quiet = now - *last_touch;
    if (quiet > within)
    {
        detail = fmt::format("nothing for {:.1f} s", std::chrono::duration<double>(quiet).count());
        return when_silent;
    }
    return State::ok;
}

void account(Continuity& counters, const HealthSnapshot* previous, const HealthSnapshot& current)
{
    if (previous == nullptr)
    {
        return;
    }
    if (current.sequence <= previous->sequence || current.uptime_ms < previous->uptime_ms)
    {
        ++counters.restarts;
        return;
    }
    counters.missed += current.sequence - previous->sequence - 1u;
}

}  // namespace node_health
