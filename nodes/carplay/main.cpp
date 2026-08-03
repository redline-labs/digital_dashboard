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
#include "node_config.h"
#include "simulate.h"
#include "usb_pipeline.h"

#include "helpers/ffmpeg_log.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <atomic>
#include <sstream>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

namespace
{

std::atomic<bool> g_stop{false};

// Set by the AirPlay receiver once a session reaches RECORD, so the idle
// session-state publisher below stops overwriting the live state.
std::atomic<bool> g_recording{false};

void handleSignal(int)
{
    g_stop.store(true);
}

}  // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");
    // libavcodec and libswscale otherwise write straight to stderr, untimed and
    // unfiltered, in the middle of our own output.
    helpers::routeFfmpegLogsToSpdlog();

    cxxopts::Options options("carplay", "Wired CarPlay driver node");
    options.add_options()
        ("key-prefix", "Zenoh key prefix for all published/subscribed topics",
         cxxopts::value<std::string>()->default_value("nodes/carplay"))
        ("config", "Node configuration YAML (see configs/carplay/carplay.yaml)",
         cxxopts::value<std::string>()->default_value(""))
        ("oem-button", "Show the manufacturer button on CarPlay's home screen",
         cxxopts::value<bool>()->default_value("true"))
        ("oem-label", "Caption under the manufacturer button (overrides --config)",
         cxxopts::value<std::string>()->default_value(""))
        ("night-mode", "Draw CarPlay's own UI in its night theme",
         cxxopts::value<bool>()->default_value("false"))
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
        ("location",
         "Static GPS fix for testing the location uplink, \"lat,lon[,alt_m,speed_kn,course_deg]\" "
         "(otherwise a GPS source publishes on <prefix>/location)",
         cxxopts::value<std::string>()->default_value(""))
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

    // Config file first, command line over the top: the flags are for a
    // bring-up session, the file is what a vehicle ships with.
    carplay::NodeConfig config;
    if (const std::string path = args["config"].as<std::string>(); !path.empty())
    {
        if (!carplay::loadNodeConfig(path, config))
        {
            // Carrying on with defaults would silently drop whatever the file
            // was configuring, and the icons are the whole point of having one.
            SPDLOG_ERROR("[node] refusing to start with an unusable --config");
            return 1;
        }
        SPDLOG_INFO("[node] loaded config from {}", path);
    }
    // count() is the number of times the flag was actually given, so these only
    // override when asked for -- a config saying `enabled: false` stands.
    if (args.count("oem-button") > 0)
    {
        config.oem_button.enabled = args["oem-button"].as<bool>();
    }
    if (args.count("night-mode") > 0)
    {
        config.night_mode = args["night-mode"].as<bool>();
    }
    if (const std::string label = args["oem-label"].as<std::string>(); !label.empty())
    {
        config.oem_button.label = label;
    }

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
            // Once the AirPlay session is live the receiver publishes the
            // authoritative state (device connected, recording); don't clobber it.
            if (!g_recording.load())
            {
                bridge.publishSession(idle);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    config.max_stage = args["max-stage"].as<int>();
    config.state_dir = args["state-dir"].as<std::string>();
    config.allow_missing_mfi = args.count("iap2-allow-missing-mfi") > 0;

    // A static GPS fix for bench-testing the location uplink: "lat,lon[,alt,speed,course]".
    if (const std::string spec = args["location"].as<std::string>(); !spec.empty())
    {
        carplay::LocationFix fix;
        double values[5] = {0, 0, 0, 0, 0};
        int parsed = 0;
        std::stringstream stream(spec);
        std::string field;
        while (parsed < 5 && std::getline(stream, field, ','))
        {
            try
            {
                values[parsed++] = std::stod(field);
            }
            catch (const std::exception&)
            {
                break;
            }
        }
        if (parsed >= 2)
        {
            fix.latitude_deg = values[0];
            fix.longitude_deg = values[1];
            fix.altitude_m = values[2];
            fix.speed_knots = values[3];
            fix.course_deg = values[4];
            fix.valid = true;
            config.static_location = fix;
            SPDLOG_INFO("[node] static test location {}, {}", fix.latitude_deg, fix.longitude_deg);
        }
        else
        {
            SPDLOG_ERROR("[node] --location needs at least \"lat,lon\"; ignoring '{}'", spec);
        }
    }

    const bool usb_ok = carplay::runUsbPipeline(config, bridge, g_stop, &g_recording);
    if (!usb_ok)
    {
        SPDLOG_ERROR("[node] USB bring-up did not complete -- see docs/carplay_bringup.md");
    }

    // Stages 5+ (iAP2/MFi, NCM, AirPlay) are not wired up yet; the pipeline
    // holds the session open until interrupted.
    g_stop.store(true);

    session_thread.join();

    SPDLOG_INFO("[node] shutting down");
    return 0;
}
