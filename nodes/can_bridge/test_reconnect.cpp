// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bridged channel whose hardware goes away is reported lost, and a channel
// opened in its place carries traffic on the same topic.
//
// An unplugged PCAN adapter used to leave the channel reporting itself running
// while it received nothing, and nothing ever reopened it. The virtual bus
// stands in for the adapter: stopping its channel is the unplug.

#include "bridged_channel.h"

#include "can/backend.h"
#include "can/virtual_backend.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_subscriber.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace
{

using namespace std::chrono_literals;
using can_bridge::ReopenBackoff;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void backoffSchedule()
{
    const auto t0 = ReopenBackoff::Clock::now();
    ReopenBackoff backoff;
    expect(backoff.due(t0), "the first reopen is tried at once");
    backoff.failed(t0);
    expect(!backoff.due(t0 + 999ms) && backoff.due(t0 + 1s), "then after a second");
    backoff.failed(t0 + 1s);
    expect(!backoff.due(t0 + 2999ms) && backoff.due(t0 + 3s), "then two more");
    for (int i = 0; i < 10; ++i)
    {
        backoff.failed(t0);
    }
    expect(!backoff.due(t0 + 29s) && backoff.due(t0 + 30s), "never more than thirty");
    backoff.reset();
    expect(backoff.due(t0), "and a success starts over");
}

helpers::CanFrame frameWithId(std::uint32_t id)
{
    helpers::CanFrame frame;
    frame.id = id;
    frame.len = 1;
    frame.data[0] = 0x42;
    return frame;
}

}  // namespace

int main()
{
    backoffSchedule();

    spdlog::set_level(spdlog::level::off);
    if (!pub_sub::SessionManager::getOrCreate())
    {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    const std::string bus = "reconnect_" + std::to_string(::getpid());
    can::Registry registry;
    registry.add(can::make_virtual_backend());

    can_bridge::ChannelConfig config;
    config.name = "test";
    config.device = "virtual:" + bus;
    config.rxKey = "test/can_bridge/reconnect/" + std::to_string(::getpid());
    config.publishRx = true;
    config.acceptTx = false;

    std::atomic<int> received{0};
    pub_sub::ZenohTypedSubscriber<::CanFrame> subscriber(
        config.rxKey, [&](::CanFrame::Reader) { received.fetch_add(1); });

    auto first = registry.open(config.device, can_bridge::openOptions(config));
    expect(first.has_value(), "opens the virtual channel");
    if (!first.has_value())
    {
        return 1;
    }
    can_bridge::BridgedChannel bridged(config, *first);
    bridged.start();
    std::this_thread::sleep_for(300ms);

    can::virtual_bus_inject(bus, frameWithId(0x100));
    std::this_thread::sleep_for(200ms);
    expect(received.load() == 1, "a frame on the bus reaches the topic");
    expect(!bridged.lost(), "a working channel is not lost");

    // The unplug.
    (void)bridged.channel()->stop();
    expect(bridged.lost(), "a channel whose hardware stopped is lost");

    auto second = registry.open(config.device, can_bridge::openOptions(config));
    expect(second.has_value(), "the channel opens again");
    if (second.has_value())
    {
        bridged.reattach(*second);
    }
    expect(!bridged.lost(), "and once reattached it is not lost");

    can::virtual_bus_inject(bus, frameWithId(0x101));
    std::this_thread::sleep_for(200ms);
    expect(received.load() == 2, "traffic flows again on the same topic");

    bridged.stop();
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
