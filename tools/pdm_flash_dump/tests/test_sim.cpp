// The whole dump against pdm32_sim serve, reached the way a remote unit is: a zenoh: channel onto the topics the
// simulator's bus is bridged to. The blocks that come back must be the images the simulator loaded, the unit's
// flash must be untouched (nothing programmed, nothing erased), and only read-only requests may have gone out.
// Skips without the firmware image or a zenoh session.
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "can/backend.h"
#include "can_zenoh/zenoh_backend.h"
#include "pdm32/top/scenario.h"
#include "pdm_flash_dump/dump.h"
#include "pdm_flash_dump/session.h"
#include "pub_sub/session_manager.h"
#include "server.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

nlohmann::json call(pdm32::Server& server, const std::string& method) {
    return nlohmann::json::parse(server.dispatcher().handleLine(
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}}.dump()))["result"];
}

}  // namespace

int main() {
    spdlog::set_level(spdlog::level::warn);
    if (!std::filesystem::exists(sim::sys::default_firmware_path())) {
        std::fprintf(stderr, "SKIP: no firmware image\n");
        return PROJECT_TEST_SKIP_CODE;
    }
    if (!pub_sub::SessionManager::getOrCreate()) {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    const std::string prefix = "test/pdm_flash_dump/p" + std::to_string(::getpid());
    const std::string scenario_path = std::string(PDM32_SCENARIO_DIR) + "/short_output.json";
    pdm32::ServeOptions options;
    options.config.can.rxKey = prefix + "/rx";
    options.config.can.txKey = prefix + "/tx";
    options.config.scenario = scenario_path;
    pdm32::Server server(options);
    std::atomic<bool> ready{false};
    std::thread loop([&] { server.run([&] { ready = true; }); });
    while (!ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    can::Registry registry;
    registry.add(can_zenoh::make_zenoh_backend());
    auto channel = registry.open("zenoh:" + prefix, can::OpenOptions{});
    check(channel.has_value(), "the zenoh: channel opens");
    if (channel) {
        pdm_flash_dump::ChannelLink link(*channel);
        pdm_flash_dump::Session session(link, std::nullopt, std::chrono::milliseconds(1000));
        try {
            session.discover(std::chrono::seconds(3));
            check(session.serial() == 1, "the simulated unit beacons serial 1");
            session.connect();
            const std::vector<pdm_flash_dump::Range> plan{{0x0462, 0x0500, "factory"},
                                                          {0x1E20, 0x2400, "config"},
                                                          {0x7B00, 0x8000, "current_cal"}};
            const auto image = pdm_flash_dump::read_plan(session, plan, {});
            const auto out = std::filesystem::temp_directory_path() / ("pdm_flash_dump_" + std::to_string(::getpid()));
            const auto manifest = pdm_flash_dump::write_dump(session, image, plan, 0, out);
            session.close_session();

            const auto loaded = sim::sys::instance_config_for(sim::sys::load_scenario(scenario_path), "").images;
            for (const char* name : {"config", "current_cal", "factory"}) {
                const auto* block = loaded.find(name);
                std::vector<uint8_t> dumped;
                if (std::ifstream in{out / (std::string(name) + ".bin"), std::ios::binary}) {
                    dumped.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
                // The dumped factory block runs to its end; the loaded one may be shorter.
                check(block != nullptr && dumped.size() >= block->bytes.size() &&
                          std::equal(block->bytes.begin(), block->bytes.end(), dumped.begin()),
                      std::string(name) + ": the dump is the image the simulator loaded");
            }
            check(manifest["config"].value("crc_ok", false) && manifest["current_cal"].value("crc_ok", false),
                  "both blocks' CRCs check");
            check(manifest["requests_sent"] == nlohmann::json::array({8, 9, 14}), "only flow, connect and read");
            std::filesystem::remove_all(out);
        } catch (const std::exception& e) {
            check(false, e.what());
        }
    }

    const auto status = call(server, "status");
    check(status["flash_bytes_programmed"] == 0 && status["flash_erases"] == 0, "the unit's flash is untouched");

    server.stop();
    loop.join();
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
