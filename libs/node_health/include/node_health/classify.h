// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a monitor concludes about one node, as a pure function of what it has
// seen and the time. No zenoh and no clock reads, so every boundary is testable.
#ifndef NODE_HEALTH_CLASSIFY_H_
#define NODE_HEALTH_CLASSIFY_H_

#include "node_health/state.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace node_health
{

enum class Verdict
{
    // The node's own report, passed through.
    ok,
    starting,
    degraded,
    fault,
    stopping,
    // Alive by its identity, but no sample for several of its periods: hung.
    late,
    // Alive by its identity, and has never reported health at all.
    silent,
    // Its identity went away after it said it was stopping: a clean exit.
    exited,
    // Its identity went away without a stopping sample: it died.
    gone,
};

std::string_view to_string(Verdict verdict);

// True for the verdicts a person does not need to act on.
bool isHealthy(Verdict verdict);

struct Observation
{
    // Whether the node directory has an identity for this session at all. A
    // health sample can arrive before the directory has caught up.
    bool identity_known = false;
    bool identity_reachable = false;
    // When this monitor first saw the identity.
    std::optional<Clock::time_point> identity_first_seen;

    std::optional<HealthSnapshot> last;
    std::optional<Clock::time_point> last_received;
};

struct ClassifyOptions
{
    // A sample older than this many of the node's periods makes it late.
    double late_factor = 3.0;
    // How long an identity may exist with no health sample before it is silent.
    std::chrono::milliseconds silent_grace{3000};
};

// Precedence, first match wins:
//   1. identity known and unreachable: exited if the last sample said
//      stopping, otherwise gone;
//   2. no sample yet: silent once silent_grace has passed since the identity
//      was first seen, otherwise starting;
//   3. the last sample is older than late_factor times its period, clamped to
//      [100 ms, 60 s] (a period of 0 counts as 1 s): late;
//   4. otherwise the state the node reported.
Verdict classify(const Observation& observation, Clock::time_point now,
                 const ClassifyOptions& options = {});

// An activity check's state: `within` since the last touch is ok; before the
// first touch it is starting until `within` has passed since `created`; after
// that, `when_silent`. `detail` says how long it has been quiet.
State activityState(std::optional<Clock::time_point> last_touch, Clock::time_point created,
                    std::chrono::milliseconds within, State when_silent, Clock::time_point now,
                    std::string& detail);

// Per-node counters a monitor keeps across samples.
struct Continuity
{
    // The node started again: its sequence or uptime went backwards.
    std::uint64_t restarts = 0;
    // Samples that never arrived, from gaps in the sequence.
    std::uint64_t missed = 0;
};

// Updates `counters` for `current` arriving after `previous`.
void account(Continuity& counters, const HealthSnapshot* previous, const HealthSnapshot& current);

}  // namespace node_health

#endif  // NODE_HEALTH_CLASSIFY_H_
