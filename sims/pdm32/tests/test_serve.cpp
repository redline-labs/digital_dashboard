// `pdm32_sim serve` in-process, on a real zenoh session and the real firmware: the PDM's data stream arrives on the
// rx topic at its configured rate, a frame published on the tx topic reaches the PDM, and the agent methods answer
// from the simulation's thread while it runs in real time. Skips without the firmware image or a zenoh session.
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "can_frame.capnp.h"
#include "firmware_path.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "server.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

}  // namespace

int main() {
    spdlog::set_level(spdlog::level::warn);
    if (sim::test::firmware_hex().empty()) {
        std::fprintf(stderr, "SKIP: no firmware image\n");
        return PROJECT_TEST_SKIP_CODE;
    }
    if (!pub_sub::SessionManager::getOrCreate()) {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    const std::string prefix = "test/pdm32_sim/p" + std::to_string(::getpid());
    pdm32::ServeOptions options;
    options.config.can.rxKey = prefix + "/rx";
    options.config.can.txKey = prefix + "/tx";
    options.config.scenario = std::string(PDM32_SCENARIO_DIR) + "/short_output.json";

    std::mutex mutex;
    std::map<uint32_t, int> counts;
    std::string channel_name;
    pub_sub::ZenohTypedSubscriber<::CanFrame> subscriber(options.config.can.rxKey, [&](::CanFrame::Reader message) {
        const std::lock_guard<std::mutex> lock(mutex);
        counts[message.getId()]++;
        channel_name = message.getChannel();
    });

    pdm32::Server server(options);
    std::atomic<bool> ready{false};
    std::thread loop([&] { server.run([&] { ready = true; }); });
    while (!ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Past power-on, then a one-second window.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        const std::lock_guard<std::mutex> lock(mutex);
        counts.clear();
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (uint32_t id = 0x500; id <= 0x505; ++id) {
            // Every 10 ms in the scenario's configuration; real time on a loaded machine is allowed some slack.
            check(counts[id] >= 80 && counts[id] <= 120,
                  "0x" + std::to_string(id) + " at its 10 ms rate: " + std::to_string(counts[id]) + " in 1 s");
        }
        check(channel_name == "can0", "frames carry the channel name the config gives (" + channel_name + ")");
    }

    // Into the PDM.
    {
        pub_sub::ZenohPublisher<::CanFrame> publisher(options.config.can.txKey);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        helpers::CanFrame frame;
        frame.id = 0x123;
        frame.len = 2;
        frame.data[0] = 0x01;
        frame.data[1] = 0xA0;
        pub_sub::toCapnp(frame, publisher.fields());
        publisher.put();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        const auto reply = nlohmann::json::parse(
            server.dispatcher().handleLine(R"({"jsonrpc":"2.0","id":1,"method":"can.recv","params":{"id":291}})"));
        const auto& frames = reply["result"]["frames"];
        check(frames.size() == 1, "a frame on the tx topic reached the PDM's bus once");
        check(frames.size() == 1 && frames[0]["data"] == "01A0" && frames[0]["from_pdm"] == false,
              "with its payload, from outside");
    }

    // The methods answer from the running simulation; run is for stepped mode only.
    {
        const auto status = nlohmann::json::parse(
            server.dispatcher().handleLine(R"({"jsonrpc":"2.0","id":2,"method":"status"})"));
        check(status["result"]["t"].get<double>() > 1.5, "status reports simulated time advancing with the wall");
        const auto run = nlohmann::json::parse(
            server.dispatcher().handleLine(R"({"jsonrpc":"2.0","id":3,"method":"run","params":{"seconds":1}})"));
        check(run["error"]["data"]["reason"] == "WRONG_MODE", "run is refused in real time");
    }

    server.stop();
    loop.join();
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
