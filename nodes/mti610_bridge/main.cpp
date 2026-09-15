// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bridges an Xsens MTi-610 onto the zenoh bus.
//
// The MTi-610 is the IMU member of the 600-series: calibrated acceleration,
// rate of turn and magnetic field, the strapdown increments, barometric
// pressure and temperature. It has NO ORIENTATION FILTER, so there is no
// quaternion and no Euler angle on any of these topics -- that is a 620, 630
// or 670. See docs/nodes/mti610_bridge.md.
//
// Modes, in the order you would use them on a bench:
//
//   --probe    open the port, identify the device, print what it is configured
//              to output, exit. The first thing to run against a new unit, and
//              the thing that says whether the baud rate is right.
//   --check    diff the device against the config file and exit non-zero on
//              drift. Usable from a health check.
//   --replay   feed a captured byte stream through the whole decode and
//              publish path with no device present. WITH NO HARDWARE THIS IS
//              THE ONLY WAY TO RUN THE NODE END TO END.
//   (default)  run.

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <cxxopts.hpp>
#include "core/core.h"
#include <spdlog/spdlog.h>

#include "mti610.capnp.h"
#include "mti610/replay_stream.h"
#include "mti610/serial_stream.h"
#include "mti610/stream_client.h"
#include "node_config.h"
#include "node_health/reporter.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "publishers.h"
#include "services.h"

namespace
{

std::atomic<bool> gRunning { true };

void handleSignal(int)
{
    gRunning.store(false);
}

using namespace mti610_node;
using namespace std::chrono_literals;

mti610::StreamClient::StreamFactory serialFactory(const DeviceConfig& device)
{
    return [port = device.port, baud = device.baud, timeout = device.openTimeoutMs]()
               -> mti610::Result<std::unique_ptr<mti610::ByteStream>> {
        mti610::Result<std::unique_ptr<mti610::SerialStream>> stream =
            mti610::SerialStream::open(port, { .baud = baud, .openTimeoutMs = timeout });

        if (!stream.has_value())
        {
            return std::unexpected(stream.error());
        }

        return std::unique_ptr<mti610::ByteStream>(std::move(*stream));
    };
}

mti610::StreamClient::StreamFactory replayFactory(const std::string& path, bool loop)
{
    return [path, loop]() -> mti610::Result<std::unique_ptr<mti610::ByteStream>> {
        mti610::Result<std::unique_ptr<mti610::ReplayStream>> stream =
            mti610::ReplayStream::open(path, { .chunkSize = 64,
                                               .loop = loop,
                                               // Paced roughly like a 115200
                                               // link, so a dashboard watching
                                               // a replay sees something that
                                               // moves at a plausible speed.
                                               .chunkDelayMs = 5 });

        if (!stream.has_value())
        {
            return std::unexpected(stream.error());
        }

        SPDLOG_INFO("mti610: replaying {} ({} bytes)", path, (*stream)->size());
        return std::unique_ptr<mti610::ByteStream>(std::move(*stream));
    };
}

mti610::StreamClient::Options streamOptions(const NodeConfig& config)
{
    mti610::StreamClient::Options options;

    options.session.replyTimeoutMs = config.configuration.replyTimeoutMs;
    options.session.retries = config.configuration.retries;
    options.session.mode = config.configuration.mode;
    options.session.policy = config.configuration.policy;

    options.desiredOutputs = to_output_entries(config.configuration.outputs);

    options.reopenBackoff.clear();
    for (std::uint32_t ms : config.device.reopenBackoffMs)
    {
        options.reopenBackoff.push_back(std::chrono::milliseconds(ms));
    }

    return options;
}

void printOutputs(const std::vector<mti610::OutputEntry>& entries)
{
    if (entries.empty())
    {
        SPDLOG_INFO("  (the device is configured to output nothing)");
        return;
    }

    for (const mti610::OutputEntry& entry : entries)
    {
        const char* name = xbus::is_known_data_id(entry.rawId)
                               ? xbus::data_name(static_cast<xbus::DataId>(
                                     xbus::data_type(entry.rawId)))
                               : "unknown -- this device is not an MTi-610";

        const std::string rate = entry.frequencyHz == xbus::kMaxFrequency
                                     ? std::string("max")
                                     : std::to_string(entry.frequencyHz) + " Hz";

        SPDLOG_INFO("  0x{:04X}  {:<20} {:<8} {}", entry.rawId, name, rate,
                    to_string(xbus::data_precision(entry.rawId)));
    }
}

// Open the port, walk the device once, print, and get out. Everything --probe
// and --check need, without a bus.
int inspectDevice(const NodeConfig& config, bool checkOnly)
{
    mti610::Result<std::unique_ptr<mti610::SerialStream>> stream = mti610::SerialStream::open(
        config.device.port,
        { .baud = config.device.baud, .openTimeoutMs = config.device.openTimeoutMs });

    if (!stream)
    {
        SPDLOG_ERROR("mti610: {}", to_string(stream.error()));
        return 1;
    }

    xbus::Framer framer;
    mti610::DeviceSession session(**stream, framer,
                                  { .replyTimeoutMs = config.configuration.replyTimeoutMs,
                                    .retries = config.configuration.retries,
                                    .mode = config.configuration.mode,
                                    .policy = config.configuration.policy });

    if (const mti610::Result<void> entered = session.goToConfig(); !entered)
    {
        SPDLOG_ERROR("mti610: cannot enter config state: {}", to_string(entered.error()));
        SPDLOG_ERROR("mti610: a device that answers nothing at all is usually the wrong baud "
                     "rate; {} is configured", config.device.baud);
        return 1;
    }

    // The port may have been full of measurement data when GoToConfig landed.
    (*stream)->flush();
    framer.reset();

    if (const mti610::Result<mti610::DeviceInfo> info = session.identify())
    {
        SPDLOG_INFO("mti610: product code   {}",
                    info->productCode.empty() ? "(not reported)" : info->productCode);
        SPDLOG_INFO("mti610: device id      {:#018x}", info->deviceId);
        SPDLOG_INFO("mti610: firmware       {}", info->firmwareVersion());
        SPDLOG_INFO("mti610: hardware       {}.{}", info->hardwareMajor, info->hardwareMinor);

        if (!info->productCode.empty() && !info->looksLikeMti610())
        {
            SPDLOG_WARN("mti610: this is not an MTi-610. Outputs this build models will still "
                        "decode; anything else lands on the raw topic.");
        }
    }
    else
    {
        SPDLOG_WARN("mti610: cannot identify the device: {}", to_string(info.error()));
    }

    const mti610::Result<std::vector<mti610::OutputEntry>> actual = session.readOutputConfig();
    if (!actual)
    {
        SPDLOG_ERROR("mti610: cannot read the output configuration: {}",
                     to_string(actual.error()));
        return 1;
    }

    SPDLOG_INFO("mti610: the device is configured to send:");
    printOutputs(*actual);

    const std::vector<mti610::OutputEntry> desired =
        to_output_entries(config.configuration.outputs);
    const std::vector<mti610::Change> changes = mti610::diff(*actual, desired);

    if (changes.empty())
    {
        SPDLOG_INFO("mti610: which is what the config file asks for");
    }
    else
    {
        for (const mti610::Change& change : changes)
        {
            SPDLOG_WARN("mti610: {}", to_string(change));
        }
    }

    // Leave the device measuring rather than parked in Config. A --probe that
    // silently stopped the device would be a diagnostic that breaks the thing
    // it was run to diagnose.
    if (const mti610::Result<void> measuring = session.goToMeasurement(); !measuring)
    {
        SPDLOG_WARN("mti610: could not return the device to measurement state: {}",
                    to_string(measuring.error()));
    }

    if (!checkOnly)
    {
        return 0;
    }

    return mti610::is_satisfied(changes, config.configuration.policy) ? 0 : 1;
}

void publishStatus(pub_sub::ZenohPublisher<::Mti610Status>& publisher,
                   const mti610::StreamClient& client, const Publishers& publishers,
                   const NodeConfig& config)
{
    const mti610::StreamClient::Stats stats = client.stats();
    const mti610::DeviceInfo info = client.deviceInfo();
    const std::vector<mti610::OutputEntry> effective = client.effectiveOutputs();
    const std::vector<mti610::Change> changes = client.lastChanges();
    const std::vector<SeenItem> seen = publishers.seen();

    auto fields = publisher.fields();

    fields.setConnected(client.running() && stats.opens > 0 && stats.lastError.empty());
    fields.setMeasuring(client.measuring());
    fields.setPort(config.device.port);
    fields.setBaud(config.device.baud);

    fields.setDeviceId(info.deviceId);
    fields.setProductCode(info.productCode);
    fields.setFirmwareVersion(info.firmwareVersion());
    fields.setHardwareVersion(std::to_string(info.hardwareMajor) + "." +
                              std::to_string(info.hardwareMinor));
    fields.setIsMti610(info.looksLikeMti610());

    fields.setOpens(stats.opens);
    fields.setBytesRead(stats.bytesRead);
    fields.setDataMessages(stats.dataMessages);
    fields.setItems(stats.items);
    fields.setUnknownItems(stats.unknownItems);
    fields.setMalformedItems(stats.malformedItems);
    fields.setTruncatedBodies(stats.truncatedBodies);
    fields.setDeviceResets(stats.deviceResets);

    fields.setFramedMessages(stats.framer.messages);
    fields.setChecksumErrors(stats.framer.checksumErrors);
    fields.setResyncs(stats.framer.resyncs);
    fields.setDroppedBytes(stats.framer.droppedBytes);
    fields.setBufferOverflows(stats.framer.overflows);

    fields.setConfigMode(config.configuration.mode == mti610::ConfigMode::Enforce
                             ? ::Mti610ConfigMode::ENFORCE
                             : ::Mti610ConfigMode::REPORT_ONLY);
    fields.setConfigChecks(stats.configChecks);
    fields.setConfigWrites(stats.configWrites);
    fields.setLastError(stats.lastError);

    auto outputs = fields.initEffectiveOutputs(static_cast<unsigned>(effective.size()));
    for (unsigned i = 0; i < effective.size(); ++i)
    {
        fill_output_entry(outputs[i], effective[i]);
    }

    auto changeList = fields.initChanges(static_cast<unsigned>(changes.size()));
    for (unsigned i = 0; i < changes.size(); ++i)
    {
        fill_change(changeList[i], changes[i]);
    }

    auto seenList = fields.initSeen(static_cast<unsigned>(seen.size()));
    for (unsigned i = 0; i < seen.size(); ++i)
    {
        seenList[i].setRawDataId(seen[i].rawDataId);
        seenList[i].setName(seen[i].name);
        seenList[i].setCount(seen[i].count);
    }

    publisher.put();
}

} // namespace

int main(int argc, char** argv)
{
    core::setupLogging({.program = "mti610_bridge"});

    std::string configPath;
    std::string replayPath;
    std::string dumpPath;
    bool debug = false;
    bool loop = false;
    bool probe = false;
    bool check = false;

    try
    {
        cxxopts::Options options("mti610_bridge", "Bridge an Xsens MTi-610 onto the zenoh bus");

        options.add_options()
            ("c,config", "YAML configuration file", cxxopts::value<std::string>(configPath))
            ("d,debug", "Verbose logging", cxxopts::value<bool>(debug))
            ("probe", "Identify the device and print its output configuration, then exit",
             cxxopts::value<bool>(probe))
            ("check", "Exit non-zero if the device does not match the config file",
             cxxopts::value<bool>(check))
            ("replay", "Replay a captured byte stream instead of opening a port",
             cxxopts::value<std::string>(replayPath))
            ("loop", "Replay the capture repeatedly", cxxopts::value<bool>(loop))
            ("dump-xbus", "Write every received byte to this file",
             cxxopts::value<std::string>(dumpPath))
            ("h,help", "Print usage");

        const cxxopts::ParseResult parsed = options.parse(argc, argv);

        if (parsed.count("help") != 0)
        {
            fmt::print("{}\n", options.help());
            return 0;
        }

        if (configPath.empty())
        {
            SPDLOG_ERROR("--config is required");
            fmt::print("{}\n", options.help());
            return 2;
        }
    }
    catch (const cxxopts::exceptions::exception& error)
    {
        SPDLOG_ERROR("{}", error.what());
        return 2;
    }

    if (debug)
    {
        spdlog::set_level(spdlog::level::debug);
    }

    NodeConfig config;
    if (!load_node_config(configPath, config))
    {
        return 1;
    }

    // Cross-field checks that cannot happen while parsing a single key.
    if (config.device.port.empty() && replayPath.empty())
    {
        SPDLOG_ERROR("[config] device.port is required unless --replay is given");
        return 1;
    }

    if (!replayPath.empty() && !mti610::is_supported_baud(config.device.baud))
    {
        // Harmless during a replay, but it would fail at the first open on
        // hardware, and reporting it now costs nothing.
        SPDLOG_WARN("[config] device.baud {} is not a rate this build can open a port at",
                    config.device.baud);
    }
    else if (replayPath.empty() && !mti610::is_supported_baud(config.device.baud))
    {
        SPDLOG_ERROR("[config] device.baud {} is not a rate this build can open a port at",
                     config.device.baud);
        return 1;
    }

    if (config.configuration.outputs.empty() && replayPath.empty())
    {
        SPDLOG_ERROR("[config] configuration.outputs is empty, so the device would be asked to "
                     "send nothing. Ask for at least one output.");
        return 1;
    }

    if (probe || check)
    {
        if (!replayPath.empty())
        {
            SPDLOG_ERROR("--probe and --check need a device; they cannot run against a capture");
            return 2;
        }
        return inspectDevice(config, check);
    }

    // Declared before any publisher, so a tool watching the bus sees the node
    // appear before its topics do.
    pub_sub::NodeIdentity identity("mti610");

    // One health topic per node, whatever it does: see libs/node_health.
    node_health::HealthReporter health("mti610");

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    Publishers publishers(config.publish.topicPrefix, config.publish.publishUnknownItems);

    mti610::StreamClient::Options options = streamOptions(config);
    mti610::StreamClient::StreamFactory factory;

    if (!replayPath.empty())
    {
        factory = replayFactory(replayPath, loop);
        options.stopWhenStreamEnds = !loop;
        // Nothing to configure in a capture, and GoToConfig would time out
        // three times before the first byte was delivered.
        options.readOnly = true;
    }
    else
    {
        factory = serialFactory(config.device);
    }

    mti610::StreamClient client(std::move(factory), options,
                                [&publishers](const xbus::MessageView& message) {
                                    publishers.publish(message);
                                });

    std::ofstream dump;
    if (!dumpPath.empty())
    {
        dump.open(dumpPath, std::ios::binary);
        if (!dump)
        {
            SPDLOG_ERROR("cannot open {} for writing", dumpPath);
            return 1;
        }

        SPDLOG_INFO("mti610: writing every received byte to {}", dumpPath);
        client.setByteTap([&dump](std::span<const std::uint8_t> bytes) {
            dump.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        });
    }

    // Services need a device to talk to. Offering them during a replay would
    // mean offering calls that always fail, which is worse than not offering
    // them.
    std::unique_ptr<Services> services;
    if (replayPath.empty())
    {
        services = std::make_unique<Services>(client, config.publish.topicPrefix,
                                              config.configuration.mode,
                                              to_output_entries(config.configuration.outputs));
    }

    pub_sub::ZenohPublisher<::Mti610Status> statusPublisher(config.publish.statusKey);

    client.start();

    auto nextStatus = std::chrono::steady_clock::now();
    auto nextRecheck = std::chrono::steady_clock::now() +
                       std::chrono::seconds(config.configuration.recheckIntervalS);

    health.markReady();

    while (gRunning.load())
    {
        health.kick();
        const auto now = std::chrono::steady_clock::now();

        if (now >= nextStatus)
        {
            publishStatus(statusPublisher, client, publishers, config);
            nextStatus = now + std::chrono::milliseconds(config.publish.statusIntervalMs);

            // Connected and measuring are different failures: a device that is
            // open but not in Measurement state is a configuration that did not
            // take, and it publishes nothing while looking attached.
            health.setCheck("serial", client.running() ? node_health::State::ok
                                                       : node_health::State::fault,
                            client.running() ? "" : "the reader is not running");
            health.setCheck("measuring", client.measuring() ? node_health::State::ok
                                                            : node_health::State::degraded,
                            client.measuring() ? "" : "open, but not in Measurement state");
        }

        if (config.configuration.recheckIntervalS != 0 && now >= nextRecheck &&
            replayPath.empty())
        {
            // Costs the data stream for as long as the pass takes; see
            // node_config.h for why the default interval is a minute.
            client.requestReconfigure();
            nextRecheck = now + std::chrono::seconds(config.configuration.recheckIntervalS);
        }

        if (!client.running())
        {
            SPDLOG_INFO("mti610: the reader stopped");
            break;
        }

        std::this_thread::sleep_for(50ms);
    }

    // Stop reading before tearing down the publishers the reader thread uses.
    client.stop();
    services.reset();

    const mti610::StreamClient::Stats stats = client.stats();
    SPDLOG_INFO("mti610: {} bytes, {} data messages, {} items ({} unknown, {} malformed), "
                "{} resyncs, {} checksum errors, {} device resets",
                stats.bytesRead, stats.dataMessages, stats.items, stats.unknownItems,
                stats.malformedItems, stats.framer.resyncs, stats.framer.checksumErrors,
                stats.deviceResets);

    return 0;
}
