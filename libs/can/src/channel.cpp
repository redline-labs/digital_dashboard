// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/channel.h"

namespace can
{

Channel::~Channel() = default;

ControllerOptions Channel::supported_controller_options() const
{
    return ControllerOptions {};
}

Result<void> Channel::set_controller_options(const ControllerOptions& options)
{
    if (auto missing = first_unsupported(options, supported_controller_options()))
    {
        return unsupported(description() + " does not support " + *missing);
    }
    return {};
}

ControllerOptions Channel::controller_options() const
{
    return ControllerOptions {};
}

Result<void> Channel::set_error_injection(const ErrorInjection& /*injection*/)
{
    return unsupported(description() + " cannot inject bus errors");
}

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
