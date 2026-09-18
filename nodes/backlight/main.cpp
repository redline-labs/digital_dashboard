// A display module's backlight and sensors, on the bus.
//
// What it reads comes from the record redline-display-setup writes at boot
// (/run/redline/displays/<role>): which backlight class device, which IIO light
// sensors and which hwmon temperature sensors belong to the module. It publishes
// all of them when something moves and every 2 s regardless, and serves one
// command -- set the brightness.
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

#include "display_backlight/deadband.h"
#include "display_backlight/display_record.h"
#include "display_backlight/sysfs.h"

#include "cli/interrupt.h"

#include "node_health/reporter.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_service.h"

#include "display_backlight.capnp.h"

#include <cxxopts.hpp>
#include "core/core.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using namespace display_backlight;

using Clock = std::chrono::steady_clock;

// Lux changes with the light, and a read costs a whole conversion; the
// temperatures move slowly. Neither is worth reading at poll_ms.
constexpr auto kLuxEvery = std::chrono::seconds(1);
constexpr auto kTemperatureEvery = std::chrono::seconds(5);
// Published at least this often, whether or not anything moved.
constexpr auto kHeartbeat = std::chrono::seconds(2);

// The light sensors, read on their own thread. An opt3001 read blocks for a
// whole conversion (~1 s each at 0.8 s integration, measured on the board), so
// reading them inline held up the status loop and, through its lock,
// set_brightness. The loop publishes whatever was read last.
class LightSampler
{
  public:
    explicit LightSampler(const std::vector<std::string>& sensors)
    {
        for (const std::string& sensor : sensors)
        {
            LightReading reading;
            reading.path = sensor;
            reading.name = readAttribute(fs::path(sensor) / "name").value_or("");
            latest_.push_back(std::move(reading));
        }
        if (!latest_.empty())
        {
            thread_ = std::thread([this] { run(); });
        }
    }

    ~LightSampler()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    LightSampler(const LightSampler&) = delete;
    LightSampler& operator=(const LightSampler&) = delete;

    std::vector<LightReading> latest() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

  private:
    void run()
    {
        std::vector<std::string> paths;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (const LightReading& reading : latest_)
            {
                paths.push_back(reading.path);
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_)
        {
            const auto pass = Clock::now();
            for (std::size_t i = 0; i < paths.size() && !stop_; ++i)
            {
                lock.unlock();
                LightReading reading = readLightSensor(paths[i]);
                lock.lock();
                latest_[i] = std::move(reading);
            }
            wake_.wait_until(lock, pass + kLuxEvery, [this] { return stop_; });
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<LightReading> latest_;
    bool stop_ = false;
    std::thread thread_;
};

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

void fillSensors(DisplayBacklightStatus::Builder& status, const std::vector<LightReading>& lights,
                 const std::vector<TemperatureReading>& readings)
{
    auto lightList = status.initLightSensors(static_cast<unsigned>(lights.size()));
    double sum = 0.0;
    std::size_t read = 0;
    for (std::size_t i = 0; i < lights.size(); ++i)
    {
        const LightReading& reading = lights[i];
        auto entry = lightList[static_cast<unsigned>(i)];
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

StatusValues valuesOf(const std::optional<BacklightStatus>& backlight, const std::vector<LightReading>& lights,
                      const std::vector<TemperatureReading>& temperatures)
{
    StatusValues values;
    values.backlight = backlight;
    for (const LightReading& reading : lights)
    {
        values.lux.push_back(reading.lux);
    }
    for (const TemperatureReading& reading : temperatures)
    {
        values.celsius.push_back(reading.celsius);
    }
    return values;
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

    // One health topic per node, whatever it does: see libs/node_health.
    node_health::HealthReporter health("backlight");

    const std::string prefix = config.resolvedTopicPrefix();
    pub_sub::ZenohPublisher<DisplayBacklightStatus> statusPublisher(prefix + "/status");

    // The service runs on a zenoh thread and the status loop on this one, and
    // both touch the backlight device. Only quick sysfs attributes are read
    // under it: the light sensors, which block, are on their own thread.
    std::mutex backlightMutex;

    pub_sub::ZenohService<DisplayBrightnessRequest, DisplayBrightnessResponse> setBrightness(
        prefix + "/set_brightness",
        [&](const DisplayBrightnessRequest::Reader& request, DisplayBrightnessResponse::Builder& response) {
            const std::lock_guard<std::mutex> lock(backlightMutex);
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

    // Before the sensor setup, so a SIGTERM during it still exits cleanly.
    cli::installInterruptHandler();

    SPDLOG_INFO("[node] {} display '{}' on {}: {} light sensor(s), {} temperature sensor(s); publishing under '{}'",
                config.role, record.name, record.connector, record.ambientLightSensors.size(),
                record.temperatureSensors.size(), prefix);

    if (config.lightIntegrationTime > 0.0)
    {
        for (const std::string& sensor : record.ambientLightSensors)
        {
            const auto set = writeLightIntegrationTime(sensor, config.lightIntegrationTime);
            if (!set)
            {
                SPDLOG_WARN("[node] {}; reading it at the driver's integration time", set.error());
            }
            else if (*set)
            {
                SPDLOG_INFO("[node] {}: integration time {} s", sensor, config.lightIntegrationTime);
            }
        }
    }

    // Listed once: the record is per boot, and so are the hwmon channels.
    std::vector<TemperatureReading> temperatures;
    for (const std::string& hwmon : record.temperatureSensors)
    {
        auto channels = findTemperatureChannels(hwmon);
        temperatures.insert(temperatures.end(), std::make_move_iterator(channels.begin()),
                            std::make_move_iterator(channels.end()));
    }

    LightSampler lights(record.ambientLightSensors);

    // A display whose brightness cannot be set still reports its sensors, so
    // this is degraded rather than a fault. `writable` never changes.
    health.setCheck("backlight", writable ? node_health::State::ok : node_health::State::degraded,
                    writable ? "" : "the backlight device is not writable");
    health.markReady();

    const auto poll = std::chrono::milliseconds(config.pollMs);
    std::optional<BacklightStatus> backlight;
    StatusValues published;
    std::optional<Clock::time_point> lastPublish;
    Clock::time_point nextBacklight = Clock::now();
    Clock::time_point nextTemperature = nextBacklight;

    // At least once a second, so the health kick keeps coming at a long poll_ms.
    const auto tick = std::min<std::chrono::milliseconds>(poll, std::chrono::seconds(1));
    cli::waitForInterrupt(
        [&] {
            health.kick();
            const auto now = Clock::now();
            if (writable && now >= nextBacklight)
            {
                nextBacklight = now + poll;
                const std::lock_guard<std::mutex> lock(backlightMutex);
                backlight = readBacklight(device);
            }
            if (now >= nextTemperature)
            {
                nextTemperature = now + kTemperatureEvery;
                readTemperatureValues(temperatures);
            }
            const std::vector<LightReading> lux = lights.latest();

            StatusValues current = valuesOf(backlight, lux, temperatures);
            if (lastPublish && now - *lastPublish < kHeartbeat && !movedPastDeadband(published, current))
            {
                return;
            }

            auto& status = statusPublisher.fields();
            status.setTimestamp(unixMillis());
            status.setRole(record.role.c_str());
            status.setConnector(record.connector.c_str());
            status.setBacklightType(record.backlightType.c_str());
            status.setWritable(writable);
            if (backlight)
            {
                fillBacklight(status, *backlight);
            }
            fillSensors(status, lux, temperatures);
            statusPublisher.put();
            SPDLOG_DEBUG("[node] status published");

            published = std::move(current);
            lastPublish = now;
        },
        tick);

    SPDLOG_INFO("[node] shutting down");
    return 0;
}
