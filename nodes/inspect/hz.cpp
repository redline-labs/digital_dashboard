#include "inspect/hz.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
#include <zenoh.hxx>

#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

#include "pub_sub/session_manager.h"

namespace {
static std::atomic<bool> g_running_hz{true};
static void handle_sigint_hz(int /*signum*/) { g_running_hz = false; }
}

int run_hz(int argc, char** argv)
{
    cxxopts::Options options("inspect hz", "Print per-second message rate for a key");
    options.add_options()
        ("k,key", "Zenoh key-expression to subscribe to", cxxopts::value<std::string>())
        ("h,help", "Print usage");

    cxxopts::ParseResult result;
    try
    {
        result = options.parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        SPDLOG_INFO("{}", options.help());
        return 1;
    }

    if (result.count("help") || !result.count("key"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const std::string keyexpr = result["key"].as<std::string>();
    SPDLOG_INFO("Measuring hz for '{}'", keyexpr);

    std::signal(SIGINT, handle_sigint_hz);

    try
    {
        auto session = pub_sub::SessionManager::getOrCreate();
        if (!session)
        {
            SPDLOG_ERROR("Failed to obtain zenoh session");
            return 1;
        }

        std::atomic<uint64_t> counter{0};

        zenoh::Subscriber<void> sub = session->declare_subscriber(
            zenoh::KeyExpr(keyexpr),
            [&](const zenoh::Sample& /*sample*/)
            {
                ++counter;
            },
            zenoh::closures::none);

        //auto last = std::chrono::steady_clock::now();
        uint64_t last_count = 0;
        while (g_running_hz)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            //auto now = std::chrono::steady_clock::now();
            //(void)now;
            uint64_t total = counter.load(std::memory_order_relaxed);
            uint64_t diff = total - last_count;
            last_count = total;
            SPDLOG_INFO("{} msgs/s", diff);
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Error measuring hz: {}", e.what());
        return 1;
    }

    SPDLOG_INFO("Exiting.");
    return 0;
}


