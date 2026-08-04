// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/channel.h"

namespace can
{

Channel::~Channel() = default;

const char* to_string(BusState state)
{
    switch (state)
    {
    case BusState::Unknown: return "unknown";
    case BusState::ErrorActive: return "error-active";
    case BusState::ErrorWarning: return "error-warning";
    case BusState::ErrorPassive: return "error-passive";
    case BusState::BusOff: return "bus-off";
    case BusState::Stopped: return "stopped";
    }
    return "unknown";
}

} // namespace can
