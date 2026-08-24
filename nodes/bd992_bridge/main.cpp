// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bridges a Trimble BD992 GNSS receiver onto the zenoh bus.
//
// The receiver is configured as a TCP server and this node connects to it --
// nothing leaves the receiver until something attaches, which keeps the
// vehicle network quiet. Two connections: one carries the GSOF report stream,
// the other carries command and report. See libs/bd992 for why.
//
// Modes, in the order you would use them on a bench:
//
//   --probe    connect to the control port, print what the receiver is
//              configured to output, exit. Answers the two things the ICD does
//              not document -- which port serves commands, and which
//              application file index holds the running configuration.
//   --check    diff the receiver against the config file and exit non-zero on
//              drift. Usable from a health check.
//   --replay   feed a captured byte stream through the whole decode and
//              publish path with no receiver present.
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
#include <spdlog/spdlog.h>

#include "bd992/control_client.h"
#include "bd992/replay_stream.h"
#include "bd992/stream_client.h"
#include "bd992/tcp_stream.h"
#include "bd992.capnp.h"
#include "node_config.h"
#include "publishers.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "services.h"

namespace
{

std::atomic<bool> gRunning { true };

void handleSignal(int)
{
    gRunning.store(false);
}

using namespace bd992_node;

bd992::StreamClient::StreamFactory tcpFactory(const ReceiverConfig& receiver, std::uint16_t port)
{
    return [host = receiver.host, port,
            timeout = std::chrono::milliseconds(receiver.connectTimeoutMs)]()
               -> bd992::Result<std::unique_ptr<bd992::ByteStream>> {
        bd992::Result<std::unique_ptr<bd992::TcpStream>> stream =
            bd992::TcpStream::connect(host, port, timeout);
        if (!stream.has_value())
        {
            return std::unexpected(stream.error());
        }
        SPDLOG_INFO("bd992: connected to {}", (*stream)->peer());
        return std::unique_ptr<bd992::ByteStream>(std::move(*stream));
    };
}

bd992::ControlClient::Options controlOptions(const NodeConfig& config)
{
    bd992::ControlClient::Options options;
    options.replyTimeout = std::chrono::milliseconds(config.configuration.replyTimeoutMs);
    options.applicationFileIndex = config.configuration.applicationFileIndex;
    options.allowRawCommands = config.configuration.allowRawCommands;
    return options;
}

bd992::StreamClient::Options streamOptions(const NodeConfig& config)
{
    bd992::StreamClient::Options options;
    options.reconnectBackoff.clear();
    for (const std::uint32_t ms : config.receiver.reconnectBackoffMs)
    {
        options.reconnectBackoff.push_back(std::chrono::milliseconds(ms));
    }
    return options;
}

void printOutputs(const gsof::appfile::ApplicationFile& file)
{
    SPDLOG_INFO("  device type 0x{:02x}, {} output message(s), {} other record(s)",
                file.control.deviceType, file.outputCount, file.otherRecordCount);

    for (std::size_t i = 0; i < file.outputCount; ++i)
    {
        const gsof::appfile::OutputMessage& message = file.outputs[i];
        if (message.isGsof)
        {
            SPDLOG_INFO("    port {:2d}  GSOF {:<20} ({:3d})  {}", static_cast<unsigned>(message.port),
                        gsof::record_name(static_cast<gsof::RecordType>(message.gsofRecordType)),
                        message.gsofRecordType, gsof::appfile::to_string(message.rate));
        }
        else
        {
            SPDLOG_INFO("    port {:2d}  {:<27}  {}", static_cast<unsigned>(message.port),
                        gsof::appfile::to_string(message.outputType), gsof::appfile::to_string(message.rate));
        }
    }
}

// Walk the application file indices and report what each holds.
//
// This exists because the ICD documents index 0 as the factory defaults and
// says nothing about which index is running. Rather than guess, print them.
int runProbe(const NodeConfig& config)
{
    bd992::ControlClient control(
        [&config]() -> bd992::Result<std::unique_ptr<bd992::ByteStream>> {
            bd992::Result<std::unique_ptr<bd992::TcpStream>> stream = bd992::TcpStream::connect(
                config.receiver.host, config.receiver.controlPort,
                std::chrono::milliseconds(config.receiver.connectTimeoutMs));
            if (!stream.has_value())
            {
                return std::unexpected(stream.error());
            }
            return std::unique_ptr<bd992::ByteStream>(std::move(*stream));
        },
        controlOptions(config));

    SPDLOG_INFO("bd992: probing {}:{}", config.receiver.host, config.receiver.controlPort);

    int found = 0;
    for (std::uint16_t index = 0; index <= 4; ++index)
    {
        const bd992::Result<gsof::appfile::ApplicationFile> file = control.readApplicationFile(index);
        if (!file.has_value())
        {
            SPDLOG_INFO("  application file {}: {}", index, bd992::to_string(file.error()));
            continue;
        }

        ++found;
        SPDLOG_INFO("  application file {}:", index);
        printOutputs(*file);
    }

    const bd992::Result<bd992::ControlClient::Reply> options = control.readOptions(0);
    if (options.has_value())
    {
        SPDLOG_INFO("  installed options: {} byte(s)", options->data.size());
    }
    else
    {
        SPDLOG_INFO("  installed options: {}", bd992::to_string(options.error()));
    }

    if (found == 0)
    {
        SPDLOG_ERROR("bd992: no application file could be read. A clean timeout on every index "
                     "means the commands reached no listener -- check that the socket at "
                     "receiver.control_port is configured on the receiver to accept INPUT, not "
                     "output only.");
        return 1;
    }

    return 0;
}

// Diff and exit non-zero on drift, without publishing anything.
int runCheck(const NodeConfig& config)
{
    bd992::ControlClient control(
        [&config]() -> bd992::Result<std::unique_ptr<bd992::ByteStream>> {
            bd992::Result<std::unique_ptr<bd992::TcpStream>> stream = bd992::TcpStream::connect(
                config.receiver.host, config.receiver.controlPort,
                std::chrono::milliseconds(config.receiver.connectTimeoutMs));
            if (!stream.has_value())
            {
                return std::unexpected(stream.error());
            }
            return std::unique_ptr<bd992::ByteStream>(std::move(*stream));
        },
        controlOptions(config));

    const ConfigPass pass = run_config_pass(control, config, true);

    if (!pass.ok)
    {
        SPDLOG_ERROR("bd992: {}", pass.error);
        return 1;
    }

    if (pass.changes.empty())
    {
        SPDLOG_INFO("bd992: the receiver is configured as the config file asks");
        return 0;
    }

    SPDLOG_WARN("bd992: {} difference(s) from the config file", pass.changes.size());
    return 1;
}

void publishStatus(pub_sub::ZenohPublisher<::Bd992Status>& publisher, const NodeConfig& config,
                   const bd992::StreamClient& stream, const Publishers& publishers,
                   const ConfigPass& lastPass, bool configChecked, std::uint64_t outputsCorrected,
                   bool controlConnected)
{
    const bd992::StreamClient::Stats stats = stream.stats();

    ::Bd992Status::Builder out = publisher.fields();

    out.setStreamConnected(stats.connected);
    out.setControlConnected(controlConnected);
    out.setReceiverHost(config.receiver.host);
    out.setStreamPort(config.receiver.streamPort);
    out.setControlPort(config.receiver.controlPort);

    out.setStreamConnects(stats.connects);
    out.setStreamConnectFailures(stats.connectFailures);
    out.setStreamDisconnects(stats.disconnects);
    out.setLastError(stats.lastError);

    out.setBytesRead(stats.bytesRead);
    out.setPackets(stats.framer.packets);
    out.setChecksumErrors(stats.framer.checksumErrors);
    out.setFramingErrors(stats.framer.framingErrors);
    out.setResyncs(stats.framer.resyncs);
    out.setDroppedBytes(stats.framer.droppedBytes);

    out.setTransmissions(stats.transmissions);
    out.setPagesDiscarded(stats.assembler.pagesDiscarded);
    out.setRecords(stats.records);
    out.setUnknownRecords(stats.unknownRecords);
    out.setMalformedRecords(stats.malformedRecords);

    const std::vector<Publishers::Seen> seen = publishers.seen();
    ::capnp::List<::Bd992RecordSeen>::Builder list = out.initSeen(static_cast<unsigned>(seen.size()));
    for (std::size_t i = 0; i < seen.size(); ++i)
    {
        ::Bd992RecordSeen::Builder entry = list[static_cast<unsigned>(i)];
        entry.setRecordType(seen[i].recordType);
        entry.setRecordName(seen[i].recordName);
        entry.setCount(seen[i].count);
        entry.setAgeMs(seen[i].ageMs);
    }

    out.setConfigMode(config.configuration.mode == ConfigMode::Enforce ? ::Bd992ConfigMode::ENFORCE
                                                                       : ::Bd992ConfigMode::REPORT_ONLY);
    out.setPortPolicy(bd992::to_string(config.configuration.portPolicy));
    out.setConfiguredPortIndex(config.configuration.portIndex);
    out.setConfigChecked(configChecked);
    out.setConfigMatches(configChecked && lastPass.ok && lastPass.changes.empty());
    out.setOutputsCorrected(outputsCorrected);

    ::capnp::List<::Bd992ConfigChange>::Builder drift =
        out.initDrift(static_cast<unsigned>(lastPass.changes.size()));
    for (std::size_t i = 0; i < lastPass.changes.size(); ++i)
    {
        fillChange(drift[static_cast<unsigned>(i)], lastPass.changes[i]);
    }

    publisher.put();
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    std::string configPath;
    std::string replayPath;
    std::string dumpPath;
    bool probe = false;
    bool check = false;
    bool loop = false;
    bool debug = false;
    unsigned replayDelayMs = 0;

    try
    {
        cxxopts::Options options("bd992_bridge", "Bridge a Trimble BD992 GNSS receiver onto zenoh");
        options.add_options()
            ("c,config", "YAML configuration file", cxxopts::value<std::string>(configPath))
            ("probe", "Print the receiver's output configuration and exit",
             cxxopts::value<bool>(probe))
            ("check", "Report configuration drift and exit non-zero if any", cxxopts::value<bool>(check))
            ("replay", "Replay a captured GSOF byte stream instead of connecting",
             cxxopts::value<std::string>(replayPath))
            ("loop", "With --replay, start again at the end of the capture", cxxopts::value<bool>(loop))
            ("replay-delay-ms",
             "With --replay, milliseconds between chunks. 0 replays as fast as the bus will take it, "
             "which is right for a test and far too fast to watch",
             cxxopts::value<unsigned>(replayDelayMs))
            ("dump-gsof", "Write the raw received bytes to a file", cxxopts::value<std::string>(dumpPath))
            ("d,debug", "Verbose logging", cxxopts::value<bool>(debug))
            ("h,help", "Print usage");

        const cxxopts::ParseResult parsed = options.parse(argc, argv);

        if (parsed.count("help") != 0)
        {
            SPDLOG_INFO("{}", options.help());
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("bd992: {}", e.what());
        return 2;
    }

    if (debug)
    {
        spdlog::set_level(spdlog::level::debug);
    }

    if (configPath.empty())
    {
        SPDLOG_ERROR("bd992: --config is required");
        return 2;
    }

    NodeConfig config;
    if (!load_node_config(configPath, config))
    {
        return 1;
    }

    // Required to connect, but not to replay -- so it is checked here rather
    // than at parse time.
    if (config.receiver.host.empty() && replayPath.empty())
    {
        SPDLOG_ERROR("bd992: receiver.host is required unless --replay is given");
        return 2;
    }

    if (probe)
    {
        return runProbe(config);
    }
    if (check)
    {
        return runCheck(config);
    }

    // Declared before any publisher, so a tool watching the bus sees the node
    // appear before its topics do.
    pub_sub::NodeIdentity identity("bd992");

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    Publishers publishers(config.publish.topicPrefix, config.publish.publishUnknownRecords);

    // Records are published from the reader thread, which is the only thread
    // that touches these publishers -- ZenohPublisher is not thread-safe.
    bd992::StreamClient::StreamFactory factory;
    bd992::StreamClient::Options options = streamOptions(config);

    if (!replayPath.empty())
    {
        bd992::ReplayStream::Options replayOptions;
        replayOptions.loop = loop;
        // Small chunks on purpose: a capture handed over in one piece would
        // exercise a case that never happens on a socket.
        replayOptions.chunkSize = 128;
        replayOptions.chunkDelayMs = replayDelayMs;

        factory = [replayPath, replayOptions]() -> bd992::Result<std::unique_ptr<bd992::ByteStream>> {
            bd992::Result<std::unique_ptr<bd992::ReplayStream>> stream =
                bd992::ReplayStream::open(replayPath, replayOptions);
            if (!stream.has_value())
            {
                return std::unexpected(stream.error());
            }
            return std::unique_ptr<bd992::ByteStream>(std::move(*stream));
        };
        options.stopWhenStreamEnds = !loop;

        SPDLOG_INFO("bd992: replaying {}{}{}", replayPath, loop ? " (looping)" : "",
                    replayDelayMs != 0 ? fmt::format(", {} ms per chunk", replayDelayMs) : "");
    }
    else
    {
        factory = tcpFactory(config.receiver, config.receiver.streamPort);
        SPDLOG_INFO("bd992: receiver {}:{} (stream), {}:{} (control)", config.receiver.host,
                    config.receiver.streamPort, config.receiver.host, config.receiver.controlPort);
    }

    bd992::StreamClient stream(std::move(factory), options,
                               [&publishers](const gsof::RawRecord& record) { publishers.publish(record); });

    std::ofstream dump;
    if (!dumpPath.empty())
    {
        dump.open(dumpPath, std::ios::binary);
        if (!dump)
        {
            SPDLOG_ERROR("bd992: cannot write {}", dumpPath);
            return 1;
        }
        stream.setByteTap([&dump](std::span<const std::uint8_t> bytes) {
            dump.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        });
        SPDLOG_INFO("bd992: writing the raw stream to {}", dumpPath);
    }

    // The control connection and the services exist only when there is a real
    // receiver: a replay has nothing to answer with, and offering services
    // that always fail would be worse than not offering them.
    std::unique_ptr<bd992::ControlClient> control;
    std::unique_ptr<Services> services;
    std::atomic<std::uint64_t> outputsCorrected { 0 };

    if (replayPath.empty())
    {
        control = std::make_unique<bd992::ControlClient>(
            tcpFactory(config.receiver, config.receiver.controlPort), controlOptions(config));

        services = std::make_unique<Services>(Services::Deps {
            control.get(),
            &publishers,
            &config,
            &outputsCorrected,
        });
    }

    pub_sub::ZenohPublisher<::Bd992Status> statusPublisher(config.publish.statusKey);

    stream.start();

    ConfigPass lastPass;
    bool configChecked = false;
    bool controlConnected = false;

    auto nextStatus = std::chrono::steady_clock::now();
    auto nextConfigCheck = std::chrono::steady_clock::now();

    while (gRunning.load())
    {
        const auto now = std::chrono::steady_clock::now();

        if (control && !config.configuration.outputs.empty() && now >= nextConfigCheck)
        {
            lastPass = run_config_pass(*control, config, false);
            configChecked = true;
            controlConnected = lastPass.ok;

            if (lastPass.written)
            {
                outputsCorrected.fetch_add(lastPass.changes.size());
            }
            if (!lastPass.ok)
            {
                SPDLOG_WARN("bd992: configuration check failed: {}", lastPass.error);
            }

            // Zero means check once, on start, and never again.
            nextConfigCheck = config.configuration.recheckIntervalS == 0
                                  ? std::chrono::steady_clock::time_point::max()
                                  : now + std::chrono::seconds(config.configuration.recheckIntervalS);
        }

        if (now >= nextStatus)
        {
            publishStatus(statusPublisher, config, stream, publishers, lastPass, configChecked,
                          outputsCorrected.load(), controlConnected);
            nextStatus = now + std::chrono::milliseconds(config.publish.statusIntervalMs);
        }

        // A replay without --loop finishes on its own; the node should exit
        // with it rather than sitting idle.
        if (!replayPath.empty() && !loop && !stream.isRunning())
        {
            SPDLOG_INFO("bd992: replay finished");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SPDLOG_INFO("bd992: shutting down");

    // Stop reading before tearing down the publishers the reader thread uses.
    stream.stop();
    services.reset();
    control.reset();

    const bd992::StreamClient::Stats stats = stream.stats();
    SPDLOG_INFO("bd992: {} transmission(s), {} record(s), {} unknown, {} resync(s)", stats.transmissions,
                stats.records, stats.unknownRecords, stats.framer.resyncs);

    return 0;
}
