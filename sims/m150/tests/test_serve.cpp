// `m150_sim serve` in-process, on a real zenoh session and an image built outside the tree: the application's 0x123
// arrives on CAN1's rx topic every 10 ms, an LTC frame published on its tx topic reaches the application and changes
// what it sends, and the methods answer from the running simulation. Skips without M150_IMAGE or a zenoh session.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "can_frame.capnp.h"
#include "host/server.h"
#include "m150/top/served.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

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
    const char* image = std::getenv("M150_IMAGE");
    if (!image || !*image) {
        std::fprintf(stderr, "SKIP: M150_IMAGE not set\n");
        return PROJECT_TEST_SKIP_CODE;
    }
    if (!pub_sub::SessionManager::getOrCreate()) {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    const std::string prefix = "test/m150_sim/p" + std::to_string(::getpid());
    using namespace sim::sys;
    ServeOptions options;
    options.schema = sim::m150::m150_serve_schema();
    options.config = default_serve_config(options.schema);
    check(options.config.can[0].enabled && !options.config.can[1].enabled && !options.config.can[2].enabled,
          "CAN1 is on by default, CAN2 and CAN3 off");
    options.config.can[0].bridge.rxKey = prefix + "/rx";
    options.config.can[0].bridge.txKey = prefix + "/tx";

    std::mutex mutex;
    std::map<std::string, int> payloads;  // of 0x123
    std::string channel_name;
    pub_sub::ZenohTypedSubscriber<::CanFrame> subscriber(options.config.can[0].bridge.rxKey, [&](::CanFrame::Reader m) {
        if (m.getId() != 0x123) {
            return;
        }
        const auto data = m.getData();
        char hex[8];
        std::snprintf(hex, sizeof hex, "%02X%02X", data.size() > 0 ? data[0] : 0, data.size() > 1 ? data[1] : 0);
        const std::lock_guard<std::mutex> lock(mutex);
        payloads[hex]++;
        channel_name = m.getChannel();
    });

    DeviceServer server(options, std::make_unique<sim::m150::M150Served>(image, sim::e200::Engine::Jit));
    std::atomic<bool> ready{false};
    std::thread loop([&] { server.run([&] { ready = true; }); });
    while (!ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        const std::lock_guard<std::mutex> lock(mutex);
        payloads.clear();
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        const std::lock_guard<std::mutex> lock(mutex);
        // every 10 ms; real time on a loaded machine is allowed some slack
        check(payloads["0000"] >= 80 && payloads["0000"] <= 120,
              "0x123 at its 10 ms rate: " + std::to_string(payloads["0000"]) + " in 1 s");
        check(channel_name == "can0", "frames carry the channel name the config gives (" + channel_name + ")");
        payloads.clear();
    }

    // LTC #1 reports lambda 0.850: the application sends 850 (0x0352)
    {
        pub_sub::ZenohPublisher<::CanFrame> publisher(options.config.can[0].bridge.txKey);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        for (int k = 0; k < 20; ++k) {
            helpers::CanFrame frame{};
            frame.id = 0x460;
            frame.len = 8;
            const uint8_t d0[8] = {0, 0x03, 0x52, 0x00, 0x10, 0x2D, 0x00, 50};
            std::copy(d0, d0 + 8, frame.data.begin());
            pub_sub::toCapnp(frame, publisher.fields());
            publisher.put();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::lock_guard<std::mutex> lock(mutex);
        check(payloads["0352"] >= 20, "the application answers LTC frames from the tx topic: " +
                                          std::to_string(payloads["0352"]) + " frames with 0352");
    }

    {
        const auto status = nlohmann::json::parse(
            server.dispatcher().handleLine(R"({"jsonrpc":"2.0","id":2,"method":"status"})"));
        check(status["result"]["t"].get<double>() > 1.5, "status reports simulated time advancing with the wall");
        check(status["result"]["can"][0]["bit_rate"] == 1000000, "CAN1 programmed to 1 Mbit/s by the application");
        const auto console = nlohmann::json::parse(
            server.dispatcher().handleLine(R"({"jsonrpc":"2.0","id":3,"method":"console"})"));
        check(console["result"]["lines"].dump().find("M150 MVP kernel") != std::string::npos,
              "the console holds the kernel's banner");
        const auto run = nlohmann::json::parse(
            server.dispatcher().handleLine(R"({"jsonrpc":"2.0","id":4,"method":"run","params":{"seconds":1}})"));
        check(run["error"]["data"]["reason"] == "WRONG_MODE", "run is refused in real time");
    }

    server.stop();
    loop.join();
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
