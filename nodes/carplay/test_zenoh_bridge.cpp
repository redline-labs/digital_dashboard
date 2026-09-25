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
#include <mutex>
#include <optional>
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
    std::mutex frame_mutex;
    std::optional<carplay::InputEvent> last_frame;
    std::promise<void> constructed;
    std::promise<void> finish;
    std::shared_future<void> finished = finish.get_future().share();
    std::atomic<bool> seeded{false};

    std::thread node([&]() {
        carplay::ZenohBridge bridge(prefix);
        bridge.setVideoSubscriberHandler([&seen_present](bool present) { seen_present = present; });
        bridge.setInputHandler([&](const carplay::InputEvent& ev) {
            ++inputs;
            if (ev.kind == carplay::InputEvent::Kind::Touch)
            {
                const std::lock_guard<std::mutex> lock(frame_mutex);
                last_frame = ev;
            }
        });
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
            input.fields().setKind(CarPlayInput::Kind::SIRI);
            input.put();
            return inputs.load() > 0;
        });
        expect(delivered, "input published on <prefix>/input reaches the installed handler");

        // A touch frame arrives whole: both fingers, their slots, and which is
        // lifting.
        const bool framed = waitFor(3s, [&]() {
            input.fields().setKind(CarPlayInput::Kind::TOUCH);
            auto contacts = input.fields().initContacts(2);
            contacts[0].setSlot(0);
            contacts[0].setX(1000);
            contacts[0].setY(2000);
            contacts[0].setDown(true);
            contacts[1].setSlot(1);
            contacts[1].setX(3000);
            contacts[1].setY(4000);
            contacts[1].setDown(false);
            input.put();
            const std::lock_guard<std::mutex> lock(frame_mutex);
            return last_frame.has_value();
        });
        expect(framed, "a touch event reaches the handler as Touch");
        const std::lock_guard<std::mutex> lock(frame_mutex);
        if (last_frame.has_value())
        {
            const auto& c = last_frame->contacts;
            expect(c.size() == 2 && c[0].slot == 0 && c[0].x == 1000 && c[0].y == 2000 && c[0].down &&
                       c[1].slot == 1 && c[1].x == 3000 && c[1].y == 4000 && !c[1].down,
                   "every contact of the frame arrives, with its slot, position and state");
        }
    }

    finish.set_value();
    node.join();

    std::fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
