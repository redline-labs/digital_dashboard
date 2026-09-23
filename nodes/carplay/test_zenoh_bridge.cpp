// SPDX-License-Identifier: GPL-3.0-or-later
//
// ZenohBridge on a real session. The case that matters is a renderer that is
// already subscribed when the bridge starts: zenoh reports that match
// synchronously, on the declaring thread, and a bridge that declared its
// listener under the lock the listener takes parked the node forever before
// iAP2 ever started. A hang is the failure, so it is caught by a watchdog.
#include "zenoh_bridge.h"

#include "pub_sub/session_manager.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>

#include <unistd.h>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

using namespace std::chrono_literals;

template <typename Pred>
bool waitFor(std::chrono::milliseconds limit, Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    if (!pub_sub::SessionManager::getOrCreate())
    {
        SPDLOG_WARN("SKIP: no zenoh session could be opened on this host");
        return PROJECT_TEST_SKIP_CODE;
    }

    const std::string prefix = "test/carplay_bridge/" + std::to_string(::getpid());

    // The renderer comes first.
    pub_sub::ZenohTypedSubscriber<CarPlayVideo> renderer(prefix + "/video", [](CarPlayVideo::Reader) {});

    std::atomic<bool> seen_present{false};
    std::atomic<int> inputs{0};
    std::promise<void> constructed;
    std::promise<void> finish;
    std::shared_future<void> finished = finish.get_future().share();
    std::atomic<bool> seeded{false};

    std::thread node([&]() {
        carplay::ZenohBridge bridge(prefix);
        bridge.setVideoSubscriberHandler([&seen_present](bool present) { seen_present = present; });
        bridge.setInputHandler([&inputs](const carplay::InputEvent&) { ++inputs; });
        bridge.setVisibilityHandler([](bool) {});
        bridge.setMicHandler([](const carplay::AudioChunk&) {});
        bridge.setLocationHandler([](const carplay::LocationFix&) {});
        seeded = waitFor(2s, [&bridge]() { return bridge.videoSubscribersPresent(); });
        constructed.set_value();
        finished.wait();

        // Detach, then publish: nothing may reach the old handler.
        bridge.setInputHandler(nullptr);
    });

    if (constructed.get_future().wait_for(5s) != std::future_status::ready)
    {
        std::fprintf(stderr, "FAIL: bridge construction + handler install hung "
                             "with a subscriber already on the video key\n");
        std::fflush(stderr);
        std::_Exit(1);  // the node thread is parked on a mutex; it cannot be joined
    }
    expect(seeded, "an existing renderer is reported by videoSubscribersPresent()");

    {
        pub_sub::ZenohPublisher<CarPlayInput> input(prefix + "/input");
        const bool delivered = waitFor(3s, [&]() {
            input.fields().setKind(CarPlayInput::Kind::TOUCH_DOWN);
            input.put();
            return inputs.load() > 0;
        });
        expect(delivered, "input published on <prefix>/input reaches the installed handler");
    }

    finish.set_value();
    node.join();

    std::fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
