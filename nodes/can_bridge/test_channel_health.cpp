// SPDX-License-Identifier: GPL-3.0-or-later
//
// A channel's health check from its published status. Every bus state is
// named, so a state added to the schema fails the switch in channel_health.h
// rather than reading as healthy.

#include "channel_health.h"

#include <capnp/message.h>

#include <cstdio>
#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

can_bridge::ChannelHealth judge(bool open, bool running, CanBusState state, const char* error,
                                bool dropping)
{
    capnp::MallocMessageBuilder message;
    auto channel = message.initRoot<CanBridgeChannelStatus>();
    channel.setOpen(open);
    channel.setRunning(running);
    channel.setState(state);
    channel.setError(error);
    return can_bridge::channelHealth(channel.asReader(), dropping);
}

}  // namespace

int main()
{
    using node_health::State;

    const auto closed = judge(false, false, CanBusState::UNKNOWN, "no such device", false);
    expect(closed.state == State::fault && closed.reason == "no such device",
           "a channel that did not open is a fault, with its error");
    const auto stopped = judge(true, false, CanBusState::ERROR_ACTIVE, "", false);
    expect(stopped.state == State::fault && stopped.reason == "not running",
           "open but not running is a fault even with nothing to say why");

    expect(judge(true, true, CanBusState::BUS_OFF, "", false).state == State::fault, "bus off");
    expect(judge(true, true, CanBusState::STOPPED, "", false).state == State::fault, "stopped");
    expect(judge(true, true, CanBusState::ERROR_PASSIVE, "", false).state == State::degraded,
           "error passive is degraded");
    expect(judge(true, true, CanBusState::ERROR_WARNING, "", false).state == State::degraded,
           "error warning is degraded");
    expect(judge(true, true, CanBusState::BUS_OFF, "", true).reason == "bus off",
           "a bus fault outranks dropping");

    expect(judge(true, true, CanBusState::ERROR_ACTIVE, "", false).state == State::ok,
           "error active and not dropping is ok");
    expect(judge(true, true, CanBusState::UNKNOWN, "", false).state == State::ok,
           "a backend that cannot report its state is not a fault");
    const auto dropping = judge(true, true, CanBusState::ERROR_ACTIVE, "", true);
    expect(dropping.state == State::degraded && dropping.reason == "dropping frames",
           "a healthy bus whose drop counters grew is degraded");

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
