// SPDX-License-Identifier: GPL-3.0-or-later
//
// The vocabulary a node reports its health in, and how several checks fold
// into one state.
#ifndef NODE_HEALTH_STATE_H_
#define NODE_HEALTH_STATE_H_

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace node_health
{

using Clock = std::chrono::steady_clock;

// Parallel to HealthState in schemas/node_health.capnp; codec.cpp maps between
// the two with a switch, so adding a value to either is a compile error there.
enum class State
{
    unknown,
    starting,
    ok,
    degraded,
    fault,
    stopping,
};

std::string_view to_string(State state);

// How bad a state is, for folding checks together: ok 0, starting and stopping
// 1, degraded and unknown 2, fault 3. Unknown ranks with degraded because a
// check nobody could evaluate is something to look at, not something to trust.
int severity(State state);

// One named thing a node checks.
struct Check
{
    std::string name;
    State state = State::unknown;
    std::string detail;
    // When `state` last changed.
    Clock::time_point since{};
};

// The worst check, with unknown reported as degraded. ok when there are none:
// a node with nothing to check is working as far as anything can tell.
State worst(std::span<const Check> checks);

// One node's report, as it travels on the wire.
struct CheckReport
{
    std::string name;
    State state = State::unknown;
    std::string detail;
    std::uint64_t state_age_ms = 0;
};

struct HealthSnapshot
{
    std::string node;
    std::string zid;
    State state = State::unknown;
    std::uint64_t sequence = 0;
    std::uint64_t uptime_ms = 0;
    std::uint32_t period_ms = 0;
    std::uint32_t pid = 0;
    std::vector<CheckReport> checks;
};

}  // namespace node_health

#endif  // NODE_HEALTH_STATE_H_
