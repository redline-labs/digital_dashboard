// SPDX-License-Identifier: GPL-3.0-or-later
//
// HealthSnapshot <-> the NodeHealth schema. No zenoh here, so both directions
// are testable without a bus.
#ifndef NODE_HEALTH_CODEC_H_
#define NODE_HEALTH_CODEC_H_

#include "node_health/state.h"

#include "node_health.capnp.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace node_health
{

// What decode() keeps from a sample, whatever the sender put in it. A monitor
// holds one snapshot per node; a node that reported ten thousand checks, or a
// detail the size of a log file, must not be able to make that unbounded.
inline constexpr std::size_t kMaxChecks = 64;
inline constexpr std::size_t kMaxTextBytes = 256;

void encode(::NodeHealth::Builder out, const HealthSnapshot& snapshot);

// Enumerants this build does not know become State::unknown, and text and the
// check list are truncated to the limits above. Never fails on a well-formed
// message.
HealthSnapshot decode(::NodeHealth::Reader in);

// From the bytes of a sample: nullopt when they are not a readable NodeHealth
// message at all.
std::optional<HealthSnapshot> decodePayload(const std::vector<std::uint8_t>& payload);

}  // namespace node_health

#endif  // NODE_HEALTH_CODEC_H_
