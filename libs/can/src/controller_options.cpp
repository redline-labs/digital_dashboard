// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/controller_options.h"

#include <array>
#include <utility>

namespace can
{
namespace
{

// One table, so the unsupported check and the printed form cannot disagree
// about which fields exist.
using Field = bool ControllerOptions::*;
constexpr std::array<std::pair<const char*, Field>, 5> kFields { {
    { "self-acknowledge", &ControllerOptions::selfAcknowledge },
    { "classic-frames-only", &ControllerOptions::classicFramesOnly },
    { "ignore-bitrate-switch-frames", &ControllerOptions::ignoreBitrateSwitchFrames },
    { "timestamp-at-start-of-frame", &ControllerOptions::timestampAtStartOfFrame },
    { "low-latency-receive", &ControllerOptions::lowLatencyReceive },
} };

} // namespace

std::optional<std::string> first_unsupported(const ControllerOptions& requested,
                                             const ControllerOptions& supported)
{
    for (const auto& [name, field] : kFields)
    {
        if (requested.*field && !(supported.*field))
        {
            return std::string(name);
        }
    }
    return std::nullopt;
}

std::string to_string(const ControllerOptions& options)
{
    std::string out;
    for (const auto& [name, field] : kFields)
    {
        if (options.*field)
        {
            out += out.empty() ? "" : ", ";
            out += name;
        }
    }
    return out.empty() ? "defaults" : out;
}

} // namespace can
