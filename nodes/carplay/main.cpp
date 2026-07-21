// SPDX-License-Identifier: GPL-3.0-or-later
//
// CarPlay driver node: owns the wired CarPlay session with the phone
// (USB config switch, usbmux/lockdown, iAP2 + MFi auth, NCM link, AirPlay)
// and bridges it onto zenoh for the dashboard widgets.
//
// Ported from the LIVI project (https://github.com/f-io/LIVI, GPL-3.0-or-later).
//
// See docs/carplay_bringup.md for the hardware bring-up procedure. Until the
// USB pipeline is verified on a Linux host, --simulate exercises the whole
// dashboard side without any hardware.

#include "zenoh_bridge.h"
#include "simulate.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

namespace
{

std::atomic<bool> g_stop{false};

void handleSignal(int)
{
    g_stop.store(true);
}

}  // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("carplay", "Wired CarPlay driver node");
    options.add_options()
        ("key-prefix", "Zenoh key prefix for all published/subscribed topics",
         cxxopts::value<std::string>()->default_value("nodes/carplay"))
        ("state-dir", "Directory for accessory identity and pair records",
         cxxopts::value<std::string>()->default_value(""))
        ("simulate", "Publish a synthetic session (no phone required) for dashboard testing")
        ("sim-width", "Simulated video width", cxxopts::value<int>()->default_value("800"))
        ("sim-height", "Simulated video height", cxxopts::value<int>()->default_value("600"))
        ("sim-fps", "Simulated video frame rate", cxxopts::value<int>()->default_value("30"))
        ("v,verbose", "Enable debug logging")
        ("h,help", "Print usage");

    const auto args = options.parse(argc, argv);
    if (args.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }
    if (args.count("verbose"))
    {
        spdlog::set_level(spdlog::level::debug);
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const std::string prefix = args["key-prefix"].as<std::string>();
    carplay::ZenohBridge bridge(prefix);

    // Input arrives from the dashboard widget on <prefix>/input.
    bridge.setInputHandler([](const carplay::InputEvent& ev) {
        SPDLOG_DEBUG("[node] input: kind={} x={} y={} code={} value={}",
                     static_cast<int>(ev.kind), ev.x, ev.y, ev.code, ev.value);
    });
    bridge.setMicHandler([](const carplay::AudioChunk& chunk) {
        SPDLOG_DEBUG("[node] mic: {} Hz / {} ch, {} bytes",
                     chunk.sample_rate_hz, chunk.channels, chunk.len);
    });

    if (args.count("simulate"))
    {
        const bool ok = carplay::runSimulation(bridge, g_stop,
                                               args["sim-width"].as<int>(),
                                               args["sim-height"].as<int>(),
                                               args["sim-fps"].as<int>());
        return ok ? 0 : 1;
    }

    SPDLOG_WARN("[node] the USB/iAP2/AirPlay pipeline is not wired up yet -- "
                "see docs/carplay_bringup.md. Use --simulate to exercise the dashboard side.");

    carplay::SessionState idle;
    while (!g_stop.load())
    {
        bridge.publishSession(idle);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    SPDLOG_INFO("[node] shutting down");
    return 0;
}
