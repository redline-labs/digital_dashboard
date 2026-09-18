// The web console: reflash, system info, node health and service calls, served
// from the board.
//
// It holds a zenoh session and serves what it reads as JSON: /api/health from
// node_health::HealthMonitor and /api/services from the liveliness
// directories. The page's Health view reads the bus itself when it can,
// through the wasm module in wasm/ and zenohd's ws/ listener; /api/health is
// its fallback. The rest -- RAUC over D-Bus, /proc and /sys, the static
// assets -- was never on the bus.

#include "http_server.h"
#include "node_config.h"
#include "service_routes.h"
#include "update_routes.h"

#include "cli/interrupt.h"
#include "core/core.h"
#include "node_health/monitor.h"
#include "node_health/reporter.h"
#include "pub_sub/node_identity.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <thread>

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

    // RAUC first: the server registers routes that hold a reference to it.
    //
    // A board whose updater is broken should still serve the rest of the
    // console -- system information and health are exactly what someone
    // diagnosing that wants -- so this reports and carries on rather than
    // exiting. /api/update/status says `rauc_available: false` and the page
    // shows why.
    web_console::UpdateRoutes updates(config);
    std::string raucError;
    if (!updates.start(raucError))
    {
        SPDLOG_WARN("[node] RAUC is not reachable ({}): reflash is unavailable, the rest is not",
                    raucError);
    }
    else
    {
        SPDLOG_INFO("[node] RAUC on the {} bus", config.raucBus);
    }

    // Watches nodes/*/health and the node directory, and classifies with the
    // same code inspect and the Qt app use -- one classifier, three consumers.
    node_health::HealthMonitor healthMonitor;
    if (!healthMonitor.isValid())
    {
        SPDLOG_WARN("[node] no bus session: /api/health will report nothing observable");
    }

    // Liveliness subscribers: built once so they have been watching before the
    // first request, not rebuilt per request knowing nothing.
    web_console::ServiceRoutes serviceRoutes;

    web_console::HttpServer server(config, updates, healthMonitor, serviceRoutes);

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

    cli::installInterruptHandler();

    // Kept current by /api/update/status, whose D-Bus call is the real test:
    // RAUC is bus-activated, so at startup all there is to judge is the bus.
    updates.onRaucHealth([&health](bool ok, const std::string& detail) {
        health.setCheck("rauc", ok ? node_health::State::ok : node_health::State::degraded, detail);
    });

    // setCheck, not addActivityCheck: an idle console with nobody browsing is
    // healthy, and an activity check would report it degraded.
    health.setCheck("http", node_health::State::ok, "");
    // Degraded, not fault: the console is doing its job, it just cannot reflash.
    health.setCheck("rauc", updates.available() ? node_health::State::ok
                                                : node_health::State::degraded,
                    updates.available() ? "" : "RAUC is not reachable on D-Bus");
    health.markReady();

    cli::waitForInterrupt([&health] { health.kick(); });

    SPDLOG_INFO("[node] shutting down");
    server.stop();
    serving.join();
    return 0;
}
