// SPDX-License-Identifier: GPL-3.0-or-later
//
// The zenoh: backend against a real bridge over a real zenoh session.
//
// A virtual bus stands in for the wire: one channel on it is bridged onto
// zenoh, a second is "another node on the bus". A zenoh: channel then has to
// see that node's frames and reach it with its own, every flag intact --
// which is the whole claim the backend makes, that a remote bus is just a bus.
//
// Then what a remote bus can be sent that a local one cannot: a stream of the
// wrong schema, and frames whose length disagrees with their payload.

#include "can_zenoh/bridged_channel.h"
#include "can_zenoh/zenoh_backend.h"

#include "can/backend.h"
#include "can/virtual_backend.h"
#include "can_bridge.capnp.h"
#include "can_frame.capnp.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace
{

using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

helpers::CanFrame fdFrame(uint32_t id)
{
    helpers::CanFrame frame;
    frame.id = id;
    frame.isExtended = true;
    frame.isFD = true;
    frame.isBRS = true;
    frame.len = 12;
    for (uint8_t i = 0; i < frame.len; ++i)
    {
        frame.data[i] = static_cast<uint8_t>(0xA0 + i);
    }
    return frame;
}

bool sameFrame(const helpers::CanFrame& a, const helpers::CanFrame& b)
{
    if (a.id != b.id || a.len != b.len || a.isExtended != b.isExtended || a.isFD != b.isFD ||
        a.isBRS != b.isBRS || a.isRTR != b.isRTR || a.isESI != b.isESI)
    {
        return false;
    }
    for (size_t i = 0; i < a.len; ++i)
    {
        if (a.data[i] != b.data[i])
        {
            return false;
        }
    }
    return true;
}

// Everything that arrives within `window`.
std::vector<helpers::CanFrame> drain(can::Channel& channel, std::chrono::milliseconds window)
{
    std::vector<helpers::CanFrame> out;
    std::array<helpers::CanFrame, 16> batch;
    const auto until = std::chrono::steady_clock::now() + window;
    while (std::chrono::steady_clock::now() < until)
    {
        auto count = channel.receive(batch, can::Duration{50});
        if (!count.has_value())
        {
            break;
        }
        out.insert(out.end(), batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(*count));
    }
    return out;
}

void badIds(const can::Registry& registry)
{
    expect(!registry.open("zenoh:", can::OpenOptions{}).has_value(), "an empty prefix is refused");
    expect(!registry.open("zenoh:vehicle/7", can::OpenOptions{}).has_value(),
           "a prefix ending in a number (read as a channel) is refused");
    expect(!registry.open("zenoh:vehicle/c*n", can::OpenOptions{}).has_value(),
           "a prefix that is not a valid key is refused");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    if (!pub_sub::SessionManager::getOrCreate())
    {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    const std::string pid = std::to_string(::getpid());
    const std::string bus = "zenoh_backend_" + pid;
    const std::string prefix = "test/can_zenoh/backend/p" + pid;

    can::Registry registry;
    registry.add(can::make_virtual_backend());
    registry.add(can_zenoh::make_zenoh_backend());

    badIds(registry);

    auto bridgedSide = registry.open("virtual:" + bus, can::OpenOptions{});
    auto otherNode = registry.open("virtual:" + bus, can::OpenOptions{});
    auto remote = registry.open("zenoh:" + prefix, can::OpenOptions{});
    expect(bridgedSide.has_value() && otherNode.has_value() && remote.has_value(), "every channel opens");
    if (!bridgedSide.has_value() || !otherNode.has_value() || !remote.has_value())
    {
        return 1;
    }

    can_zenoh::BridgeConfig config;
    config.name = "bench";
    config.device = "virtual:" + bus;
    config.rxKey = prefix + "/rx";
    config.txKey = prefix + "/tx";
    can_zenoh::BridgedChannel bridge(config, *bridgedSide);
    bridge.start();
    std::this_thread::sleep_for(300ms);

    // --- the bus, seen from afar -------------------------------------------
    const helpers::CanFrame inbound = fdFrame(0x18FF0001);
    expect((*otherNode)->send(inbound).has_value(), "the other node transmits");
    {
        const auto got = drain(**remote, 500ms);
        expect(got.size() == 1, "a frame on the bus reaches the zenoh: channel once");
        expect(!got.empty() && sameFrame(got.front(), inbound), "with id, length, payload and FD flags intact");
    }

    // --- and reached from afar ---------------------------------------------
    const helpers::CanFrame outbound = fdFrame(0x18FF0002);
    expect((*remote)->send(outbound).has_value(), "the zenoh: channel transmits");
    {
        const auto got = drain(**otherNode, 500ms);
        expect(got.size() == 1, "a frame sent through zenoh: reaches the bus once");
        expect(!got.empty() && sameFrame(got.front(), outbound), "intact");
    }
    expect(drain(**remote, 200ms).empty(), "a frame sent is not received back, as on real hardware");
    expect((*remote)->statistics().txFrames == 1 && (*remote)->statistics().rxFrames == 1,
           "statistics count what passed through");

    // --- what only a remote bus can be sent ----------------------------------
    {
        // A stream of the wrong schema on the rx key decodes as nothing at
        // all, rather than as frames made of another message's bytes.
        const std::string wrong = "test/can_zenoh/backend_wrong/p" + pid;
        auto victim = registry.open("zenoh:" + wrong, can::OpenOptions{});
        expect(victim.has_value(), "a second zenoh: channel opens");
        pub_sub::ZenohPublisher<CanBridgeStatus> impostor(wrong + "/rx");
        std::this_thread::sleep_for(300ms);
        impostor.fields().initChannels(1)[0].setName("not a frame");
        impostor.put();
        expect(victim.has_value() && drain(**victim, 400ms).empty(), "a wrong-schema stream delivers nothing");
    }
    {
        const std::string lying = "test/can_zenoh/backend_len/p" + pid;
        auto channel = registry.open("zenoh:" + lying, can::OpenOptions{});
        expect(channel.has_value(), "a third zenoh: channel opens");
        pub_sub::ZenohPublisher<::CanFrame> publisher(lying + "/rx");
        std::this_thread::sleep_for(300ms);

        // Claims 40 bytes, carries 3.
        auto& fields = publisher.fields();
        fields.setId(0x123);
        fields.setLen(40);
        const std::array<uint8_t, 3> few{1, 2, 3};
        fields.setData(kj::arrayPtr(few.data(), few.size()));
        publisher.put();

        // Claims 255 bytes, carries 100.
        std::array<uint8_t, 100> many{};
        many.fill(0x5A);
        fields.setId(0x124);
        fields.setLen(255);
        fields.setData(kj::arrayPtr(many.data(), many.size()));
        publisher.put();

        const auto got = channel.has_value() ? drain(**channel, 500ms) : std::vector<helpers::CanFrame>{};
        expect(got.size() == 2, "both arrive");
        expect(got.size() == 2 && got[0].len == 3, "a length past the payload is cut to the payload");
        expect(got.size() == 2 && got[1].len == 64, "and never past what a frame holds");
    }

    // --- the channel's own state ---------------------------------------------
    expect((*remote)->set_listen_only(true).has_value(), "listen-only can be set");
    expect(!(*remote)->send(outbound).has_value(), "a listen-only channel refuses to transmit");
    expect(!(*remote)->set_bitrate(can::Bitrate{}).has_value(), "a remote bus's bit rate cannot be set here");
    {
        std::thread stopper([&] {
            std::this_thread::sleep_for(100ms);
            (void)(*remote)->stop();
        });
        std::array<helpers::CanFrame, 4> batch;
        const auto t0 = std::chrono::steady_clock::now();
        (void)(*remote)->receive(batch, can::Duration{5000});
        expect(std::chrono::steady_clock::now() - t0 < 2s, "stop() wakes a blocked receive");
        stopper.join();
    }

    bridge.stop();
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
