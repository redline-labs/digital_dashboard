// The web console: reflash, system info, node health and service calls, served
// from the board.
//
// What this node does NOT do, and why. It does not carry bus traffic for the
// UI: the browser is a real zenoh client, talking to zenohd over ws:// with a
// wasm module built from this tree's own schemas, so there is no gateway here
// to keep in step with the bus. It keeps a zenoh session for exactly one
// reason -- NodeIdentity and HealthReporter, so the console itself appears in
// the health view it serves.
//
// What is left is the half that was never on the bus: RAUC over D-Bus, /proc
// and /sys, the static assets, and the schema descriptors the browser loads.

#include "http_server.h"
#include "node_config.h"

#include "core/core.h"
#include "node_health/reporter.h"
#include "pub_sub/node_identity.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <string>
#include <thread>

namespace
{

std::atomic<bool> gRunning { true };

void onSignal(int)
{
    gRunning = false;
}

}  // namespace

int main(int argc, char** argv)
{
    core::setupLogging({.program = "web_console"});

    cxxopts::Options options("web_console",
                             "Web console: serves the board's system information, node health, "
                             "service calls and RAUC reflash");
    options.add_options()
        ("c,config", "YAML configuration file", cxxopts::value<std::string>())
        ("check", "Parse the configuration, report problems and exit")
        ("v,verbose", "Log every request")
        ("h,help", "Print usage");

    cxxopts::ParseResult parsed;
    try
    {
        parsed = options.parse(argc, argv);
    }
    catch (const std::exception& error)
    {
        SPDLOG_ERROR("{}", error.what());
        return 2;
    }

    if (parsed.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    spdlog::set_level(parsed.count("verbose") ? spdlog::level::debug : spdlog::level::info);

    web_console::NodeConfig config;
    if (parsed.count("config"))
    {
        if (!web_console::load_node_config(parsed["config"].as<std::string>(), config))
        {
            return 2;
        }
    }
    else
    {
        SPDLOG_INFO("[node] no --config given; using defaults ({}:{})", config.bindAddress, config.port);
    }

    // --check is what the operator config story is built on: validate a file
    // without starting anything.
    if (parsed.count("check"))
    {
        SPDLOG_INFO("[node] configuration is valid");
        return 0;
    }

    // Said once, loudly, rather than assumed to be deliberate. This is the only
    // thing standing between the network and a reflash endpoint.
    std::error_code ec;
    if (config.tokenFile.empty())
    {
        SPDLOG_WARN("[node] no token_file configured: every request is accepted. Do not do this on a "
                    "board that is reachable by anything you do not trust.");
    }
    else if (!std::filesystem::exists(config.tokenFile, ec))
    {
        SPDLOG_WARN("[node] token_file '{}' does not exist yet: every request is accepted until it does",
                    config.tokenFile);
    }

    web_console::HttpServer server(config);

    // BIND BEFORE READY. A taken port has to fail here, while this is still the
    // main thread and the exit code means something -- not after markReady(),
    // where systemd would see a healthy service that is not listening.
    if (!server.bind())
    {
        return 1;
    }
    SPDLOG_INFO("[node] listening on {}:{}", config.bindAddress, server.boundPort());

    std::thread serving([&server] { server.serve(); });

    // Announce this process so tools can put a name to the session id, and so
    // the console appears in its own health view.
    pub_sub::NodeIdentity nodeIdentity("web_console");
    node_health::HealthReporter health("web_console");

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // setCheck, not addActivityCheck: an idle console with nobody browsing is
    // healthy, and an activity check would report it degraded.
    health.setCheck("http", node_health::State::ok, "");
    health.markReady();

    while (gRunning)
    {
        health.kick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SPDLOG_INFO("[node] shutting down");
    server.stop();
    serving.join();
    return 0;
}
