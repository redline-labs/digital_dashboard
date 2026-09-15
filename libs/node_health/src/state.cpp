// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/state.h"

#include <algorithm>

namespace node_health
{

std::string_view to_string(State state)
{
    switch (state)
    {
        case State::unknown:
            return "unknown";
        case State::starting:
            return "starting";
        case State::ok:
            return "ok";
        case State::degraded:
            return "degraded";
        case State::fault:
            return "fault";
        case State::stopping:
            return "stopping";
    }
    return "unknown";
}

int severity(State state)
{
    switch (state)
    {
        case State::ok:
            return 0;
        case State::starting:
        case State::stopping:
            return 1;
        case State::unknown:
        case State::degraded:
            return 2;
        case State::fault:
            return 3;
    }
    return 2;
}

State worst(std::span<const Check> checks)
{
    State result = State::ok;
    for (const Check& check : checks)
    {
        const State folded = check.state == State::unknown ? State::degraded : check.state;
        if (severity(folded) > severity(result))
        {
            result = folded;
        }
    }
    return result;
}

}  // namespace node_health
