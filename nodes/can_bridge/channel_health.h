#ifndef CAN_BRIDGE_CHANNEL_HEALTH_H_
#define CAN_BRIDGE_CHANNEL_HEALTH_H_

#include "node_health/state.h"

#include "can_bridge.capnp.h"

#include <string>

namespace can_bridge
{

struct ChannelHealth
{
    node_health::State state;
    std::string reason;
};

// A channel's health check, from the status the bridge publishes for it.
//
// `dropping` is whether its drop counters grew since the last status: growth
// matters more than the total, because a bridge that dropped frames an hour ago
// and none since is working now.
inline ChannelHealth channelHealth(CanBridgeChannelStatus::Reader channel, bool dropping)
{
    using node_health::State;

    if (!channel.getOpen() || !channel.getRunning())
    {
        return {State::fault, channel.getError().size() != 0
                                  ? std::string(channel.getError().cStr())
                                  : std::string("not running")};
    }

    switch (channel.getState())
    {
        case CanBusState::BUS_OFF:
        case CanBusState::STOPPED:
            return {State::fault, "bus off"};
        case CanBusState::ERROR_PASSIVE:
        case CanBusState::ERROR_WARNING:
            return {State::degraded, "bus errors"};
        case CanBusState::ERROR_ACTIVE:
        case CanBusState::UNKNOWN:
            break;
    }
    // After the switch, not in a default: a new bus state must be named above.
    if (dropping)
    {
        return {State::degraded, "dropping frames"};
    }
    return {State::ok, ""};
}

}  // namespace can_bridge

#endif  // CAN_BRIDGE_CHANNEL_HEALTH_H_
