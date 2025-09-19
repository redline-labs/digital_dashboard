#include "inspect/info.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <zenoh.hxx>

#include <atomic>
#include <chrono>
#include <csignal>
#include <optional>
#include <string>
#include <thread>

#include "pub_sub/session_manager.h"

namespace {
static std::atomic<bool> g_running_info{true};
static void handle_sigint_info(int /*signum*/) { g_running_info = false; }
}

int run_info(int argc, char** argv)
{
    cxxopts::Options options("inspect info", "Display information about a zenoh key");
    options.add_options()
        ("k,key", "Zenoh key-expression to inspect", cxxopts::value<std::string>())
        ("h,help", "Print usage");

    cxxopts::ParseResult result;
    try {
        result = options.parse(argc, argv);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("{}", e.what());
        SPDLOG_INFO("{}", options.help());
        return 1;
    }

    if (result.count("help") || !result.count("key")) {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const std::string keyexpr = result["key"].as<std::string>();
    SPDLOG_INFO("Inspecting key '{}'", keyexpr);

    std::signal(SIGINT, handle_sigint_info);

    try {
        auto session = pub_sub::SessionManager::getOrCreate();
        if (!session) {
            SPDLOG_ERROR("Failed to obtain zenoh session");
            return 1;
        }

        // Strategy:
        // - Try a one-shot subscriber to capture one sample and read encoding schema
        //   and any available source info (if built with unstable API).
        std::optional<std::string> schema_name;
        std::optional<std::string> publisher_id;

        auto handler = [&](const zenoh::Sample& sample) {
            try {
                schema_name = sample.get_encoding().as_string();
#if defined(ZENOHCXX) && defined(Z_FEATURE_UNSTABLE_API)
                // Source info unstable API (zenoh-c only)
                auto si = sample.get_source_info();
                publisher_id = si.id().id().to_string();
#endif
            } catch (const std::exception& e) {
                SPDLOG_WARN("Error processing sample: {}", e.what());
            }
            g_running_info = false; // we only need one sample
        };

        zenoh::Subscriber<void> sub = session->declare_subscriber(zenoh::KeyExpr(keyexpr), handler, zenoh::closures::none);

        // Wait briefly for a message to arrive
        auto start = std::chrono::steady_clock::now();
        while (g_running_info && (std::chrono::steady_clock::now() - start) < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (schema_name.has_value()) {
            SPDLOG_INFO("schema: {}", *schema_name);
        } else {
            SPDLOG_INFO("schema: (unknown - no sample received)");
        }

#if defined(Z_FEATURE_UNSTABLE_API)
        if (publisher_id.has_value()) {
            SPDLOG_INFO("publisher: {}", *publisher_id);
        } else {
            SPDLOG_INFO("publisher: (unknown)");
        }
#else
        SPDLOG_INFO("publisher info: (unavailable - zenoh source info requires unstable API)");
#endif

        return 0;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Error inspecting key: {}", e.what());
        return 1;
    }
}


