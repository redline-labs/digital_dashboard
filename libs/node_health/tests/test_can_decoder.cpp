// SPDX-License-Identifier: GPL-3.0-or-later
//
// runCanDecoder() on a real session: every frame reaches the decoder with its
// id, length and bytes, and SIGTERM ends the loop promptly -- it is every CAN
// decoder node's main loop, so a hang here is a unit systemd has to kill.
#include "node_health/can_decoder.h"

#include "pub_sub/can_frame.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"

#include "check.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{

constexpr const char* kKey = "test/node_health/can_decoder/rx";

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    if (pub_sub::SessionManager::getOrCreate() == nullptr)
    {
        SPDLOG_WARN("SKIP: no zenoh session could be opened on this host");
        return PROJECT_TEST_SKIP_CODE;
    }

    std::mutex mutex;
    std::vector<helpers::CanFrame> seen;
    std::atomic<bool> finished{false};

    std::thread bus([&] {
        pub_sub::ZenohPublisher<CanFrame> publisher(kKey);
        std::this_thread::sleep_for(300ms);  // discovery
        for (std::uint32_t i = 0; i < 5; ++i)
        {
            helpers::CanFrame frame;
            frame.id = 0x100u + i;
            frame.len = 3;
            frame.data[0] = 0xaa;
            frame.data[2] = static_cast<std::uint8_t>(i);
            pub_sub::toCapnp(frame, publisher.fields());
            publisher.put();
            std::this_thread::sleep_for(20ms);
        }
        std::this_thread::sleep_for(300ms);
        std::raise(SIGTERM);
    });

    const auto start = std::chrono::steady_clock::now();
    node_health::runCanDecoder("can_decoder_test", kKey, [&](const helpers::CanFrame& frame) {
        const std::lock_guard<std::mutex> lock(mutex);
        seen.push_back(frame);
        return frame.id == 0x102u;
    });
    finished = true;
    const auto elapsed = std::chrono::steady_clock::now() - start;
    bus.join();

    test::check(finished, "SIGTERM ends the loop");
    test::check(elapsed < 3s, "and promptly, not at some later period");
    test::check(seen.size() == 5, "every frame reaches the decoder (" + std::to_string(seen.size()) + ")");
    if (seen.size() == 5)
    {
        test::check(seen[2].id == 0x102u && seen[2].len == 3 && seen[2].data[0] == 0xaa && seen[2].data[2] == 2,
                    "with its id, length and bytes");
    }
    return test::finish();
}
