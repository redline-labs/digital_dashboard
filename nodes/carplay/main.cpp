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

#ifdef CARPLAY_HAVE_APPLE_USB
#include "usb_pipeline.h"
#endif

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
        ("max-stage", "Highest docs/carplay_bringup.md stage to attempt (2-7)",
         cxxopts::value<int>()->default_value("7"))
        ("iap2-allow-missing-mfi",
         "Continue iAP2 identification without the MFi coprocessor (CarPlay will not start)")
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

    // Keep the dashboard fed with idle session state while the USB pipeline
    // runs; the widgets should show "no session" rather than nothing at all.
    std::thread session_thread([&bridge]() {
        carplay::SessionState idle;
        while (!g_stop.load())
        {
            bridge.publishSession(idle);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

#ifdef CARPLAY_HAVE_APPLE_USB
    carplay::UsbPipelineOptions usb_options;
    usb_options.max_stage = args["max-stage"].as<int>();
    usb_options.state_dir = args["state-dir"].as<std::string>();
    usb_options.allow_missing_mfi = args.count("iap2-allow-missing-mfi") > 0;

    const bool usb_ok = carplay::runUsbPipeline(usb_options, bridge, g_stop);
    if (!usb_ok)
    {
        SPDLOG_ERROR("[node] USB bring-up did not complete -- see docs/carplay_bringup.md");
    }

    // Stages 5+ (iAP2/MFi, NCM, AirPlay) are not wired up yet; the pipeline
    // holds the session open until interrupted.
    g_stop.store(true);
#else
    SPDLOG_WARN("[node] built without apple_usb (Linux only) -- "
                "use --simulate to exercise the dashboard side.");
    while (!g_stop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
#endif

    session_thread.join();

    SPDLOG_INFO("[node] shutting down");
    return 0;
}
