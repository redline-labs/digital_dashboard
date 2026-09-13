// A display module's backlight and sensors, on the bus.
//
// What it reads comes from the record redline-display-setup writes at boot
// (/run/redline/displays/<role>): which backlight class device, which IIO light
// sensors and which hwmon temperature sensors belong to the module. It publishes
// all of them every poll period, and serves one command -- set the brightness.
//
// What it deliberately does NOT do:
//
//   - Turn the backlight on or off. The panel needs valid video before LED_EN,
//     and redline-display-backlight.service sequences that. This node writes
//     `brightness` and nothing else.
//   - Dim automatically. The lux-to-brightness curve is a product decision still
//     to be made; the sensors are published so whatever makes it can see them.
//     redline-display monitor has a placeholder curve and must stay disabled
//     while anything calls set_brightness, or the two will fight.
//   - Detect anything. No record means no module in that slot this boot, which
//     is the normal state of every machine without one: say so and exit 0.

#include "node_config.h"

#include "display_backlight/display_record.h"
#include "display_backlight/sysfs.h"

#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_service.h"

#include "display_backlight.capnp.h"

#include <cxxopts.hpp>
#include "core/core.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using namespace display_backlight;

std::atomic<bool> gRunning { true };

void onSignal(int)
{
    gRunning = false;
}

uint64_t unixMillis()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

void fillBacklight(DisplayBacklightStatus::Builder& status, const BacklightStatus& backlight)
{
    const uint32_t max = backlight.maxBrightness.value_or(0u);
    const uint32_t actual = backlight.actualBrightness.value_or(backlight.brightness.value_or(0u));

    status.setStatusValid(backlight.maxBrightness.has_value() && backlight.brightness.has_value());
    status.setMaxBrightness(max);
    status.setBrightness(backlight.brightness.value_or(0u));
    status.setActualBrightness(actual);
    status.setPercent(static_cast<float>(rawToPercent(actual, max)));
    status.setBlPower(backlight.blPower.value_or(0u));

    if (backlight.faults)
    {
        auto faults = status.initFaults(static_cast<unsigned>(backlight.faults->size()));
        for (std::size_t i = 0; i < backlight.faults->size(); ++i)
        {
            faults.set(static_cast<unsigned>(i), (*backlight.faults)[i]);
        }
    }
    if (backlight.fsmState)
    {
        status.setFsmState(backlight.fsmState->code);
        status.setFsmStateName(backlight.fsmState->name.c_str());
    }
    status.setLedCurrent(backlight.ledCurrent.value_or(0u));
    status.setPwmOutput(backlight.pwmOutput.value_or(0u));
    status.setBoost(backlight.boost.value_or(0u));
}

void fillSensors(DisplayBacklightStatus::Builder& status, const DisplayRecord& record)
{
    auto lights = status.initLightSensors(static_cast<unsigned>(record.ambientLightSensors.size()));
    double sum = 0.0;
    std::size_t read = 0;
    for (std::size_t i = 0; i < record.ambientLightSensors.size(); ++i)
    {
        const LightReading reading = readLightSensor(record.ambientLightSensors[i]);
        auto entry = lights[static_cast<unsigned>(i)];
        entry.setPath(reading.path.c_str());
        entry.setName(reading.name.c_str());
        entry.setOk(reading.lux.has_value());
        if (reading.lux)
        {
            entry.setLux(static_cast<float>(*reading.lux));
            sum += *reading.lux;
            ++read;
        }
    }
    status.setLuxAverage(read > 0 ? static_cast<float>(sum / static_cast<double>(read))
                                  : std::numeric_limits<float>::quiet_NaN());

    std::vector<TemperatureReading> readings;
    for (const std::string& hwmon : record.temperatureSensors)
    {
        auto channels = readTemperatures(hwmon);
        readings.insert(readings.end(), std::make_move_iterator(channels.begin()),
                        std::make_move_iterator(channels.end()));
    }
    auto temperatures = status.initTemperatures(static_cast<unsigned>(readings.size()));
    for (std::size_t i = 0; i < readings.size(); ++i)
    {
        auto entry = temperatures[static_cast<unsigned>(i)];
        entry.setPath(readings[i].path.c_str());
        entry.setChannel(readings[i].channel.c_str());
        entry.setName(readings[i].name.c_str());
        entry.setLabel(readings[i].label.c_str());
        entry.setOk(readings[i].celsius.has_value());
        if (readings[i].celsius)
        {
            entry.setCelsius(static_cast<float>(*readings[i].celsius));
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    core::setupLogging({.program = "backlight"});

    cxxopts::Options options("backlight",
                             "Display backlight node: publishes the module's backlight, light and "
                             "temperature sensors, and sets its brightness");
    options.add_options()
        ("c,config", "YAML configuration file", cxxopts::value<std::string>())
        ("v,verbose", "Log every status read")
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

    backlight_node::NodeConfig config;
    if (parsed.count("config"))
    {
        if (!backlight_node::load_node_config(parsed["config"].as<std::string>(), config))
        {
            return 2;
        }
    }
    else
    {
        SPDLOG_INFO("[node] no --config given; using defaults ({} display, records in '{}')", config.role,
                    config.recordDir);
    }

    const fs::path recordPath = fs::path(config.recordDir) / config.role;
    auto loaded = loadDisplayRecord(recordPath);
    if (!loaded)
    {
        if (loaded.error().kind == RecordError::Kind::missing)
        {
            // Exit 0, not an error: under Restart=on-failure an error here would
            // be retried on every board that simply has no panel in this slot.
            SPDLOG_INFO("[node] no display record at '{}': no display module was detected in the {} slot "
                        "this boot. Nothing to do.",
                        recordPath.string(), config.role);
            return 0;
        }
        SPDLOG_ERROR("[node] {}", loaded.error().message);
        return 1;
    }
    const DisplayRecord record = std::move(loaded.value());
    for (const std::string& warning : record.warnings)
    {
        SPDLOG_WARN("[node] {}: {}", recordPath.string(), warning);
    }

    const fs::path device = record.backlightDevice;
    std::error_code ec;
    const bool writable = !record.backlightDevice.empty() && !record.backlightViaSerializer() &&
                          fs::is_directory(device, ec);

    if (record.backlightViaSerializer())
    {
        SPDLOG_WARN("[node] the {} backlight has no kernel driver (device=serializer); redline-display drives "
                    "it through the serializer, so brightness cannot be set from here. Sensors are still "
                    "published.",
                    config.role);
    }
    else if (record.backlightDevice.empty())
    {
        SPDLOG_WARN("[node] the {} display's record names no backlight device", config.role);
    }
    else if (!writable)
    {
        SPDLOG_WARN("[node] backlight device '{}' is not there; brightness cannot be set", device.string());
    }
    else
    {
        const BacklightStatus initial = readBacklight(device);
        if (record.backlightMax && initial.maxBrightness && *record.backlightMax != *initial.maxBrightness)
        {
            SPDLOG_WARN("[node] the record says max={} but {} says max_brightness={}; using the driver's",
                        *record.backlightMax, (device / "max_brightness").string(), *initial.maxBrightness);
        }
        SPDLOG_INFO("[node] {} backlight at {} ({}/{})", record.backlightType, device.string(),
                    initial.actualBrightness.value_or(initial.brightness.value_or(0u)),
                    initial.maxBrightness.value_or(0u));
    }

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps.
    pub_sub::NodeIdentity nodeIdentity("backlight");

    const std::string prefix = config.resolvedTopicPrefix();
    pub_sub::ZenohPublisher<DisplayBacklightStatus> statusPublisher(prefix + "/status");

    // The service runs on a zenoh thread and the status loop on this one. Both
    // read the device, and the publisher is not thread safe; nothing here is
    // slow enough to be worth finer locking than one mutex.
    std::mutex mutex;

    pub_sub::ZenohService<DisplayBrightnessRequest, DisplayBrightnessResponse> setBrightness(
        prefix + "/set_brightness",
        [&](const DisplayBrightnessRequest::Reader& request, DisplayBrightnessResponse::Builder& response) {
            const std::lock_guard<std::mutex> lock(mutex);
            response.setOk(false);

            if (!writable)
            {
                response.setMessage(record.backlightViaSerializer()
                                        ? "this backlight is driven through the serializer, not a kernel device"
                                        : "no backlight device to write");
                return;
            }

            const BacklightStatus current = readBacklight(device);
            if (!current.maxBrightness || *current.maxBrightness == 0u)
            {
                response.setMessage("max_brightness cannot be read");
                return;
            }
            const uint32_t max = *current.maxBrightness;

            const double value = request.getValue();
            if (!std::isfinite(value) || value < 0.0)
            {
                response.setMessage("value must be a finite, non-negative number");
                return;
            }

            uint32_t requested = 0u;
            const auto unit = request.getUnit();
            if (unit == DisplayBrightnessRequest::Unit::PERCENT)
            {
                requested = percentToRaw(value, max);
            }
            else if (unit == DisplayBrightnessRequest::Unit::RAW)
            {
                requested = value >= static_cast<double>(max) ? max : static_cast<uint32_t>(std::llround(value));
            }
            else
            {
                response.setMessage("unit must be percent or raw");
                return;
            }

            const uint32_t floor = std::max<uint32_t>(1u, percentToRaw(config.minPercent, max));
            const uint32_t applied = std::clamp(requested, floor, max);

            if (auto written = writeBrightness(device, applied); !written)
            {
                SPDLOG_ERROR("[backlight] {}", written.error());
                response.setMessage(written.error().c_str());
                return;
            }

            response.setOk(true);
            response.setAppliedRaw(applied);
            response.setAppliedPercent(static_cast<float>(rawToPercent(applied, max)));
            if (applied != requested)
            {
                const std::string note = "clamped " + std::to_string(requested) + " to " + std::to_string(applied) +
                                         " (floor " + std::to_string(floor) + ", max " + std::to_string(max) + ")";
                response.setMessage(note.c_str());
            }
            SPDLOG_INFO("[backlight] brightness {} / {} ({:.1f}%)", applied, max, rawToPercent(applied, max));
        });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    SPDLOG_INFO("[node] {} display '{}' on {}: {} light sensor(s), {} temperature sensor(s); publishing under '{}'",
                config.role, record.name, record.connector, record.ambientLightSensors.size(),
                record.temperatureSensors.size(), prefix);

    auto nextStatus = std::chrono::steady_clock::now();
    while (gRunning)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextStatus)
        {
            nextStatus = now + std::chrono::milliseconds(config.pollMs);

            const std::lock_guard<std::mutex> lock(mutex);
            auto& status = statusPublisher.fields();
            status.setTimestamp(unixMillis());
            status.setRole(record.role.c_str());
            status.setConnector(record.connector.c_str());
            status.setBacklightType(record.backlightType.c_str());
            status.setWritable(writable);
            if (writable)
            {
                fillBacklight(status, readBacklight(device));
            }
            fillSensors(status, record);
            statusPublisher.put();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SPDLOG_INFO("[node] shutting down");
    return 0;
}
