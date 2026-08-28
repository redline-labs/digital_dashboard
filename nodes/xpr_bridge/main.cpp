// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bridges a Motorola MOTOTRBO radio (an XPR 5550, over its USB-RNDIS link)
// onto the zenoh bus.
//
// The radio is a network device: plugging it in brings up an Ethernet
// interface and the radio answers XNL on 192.168.10.1:8002. This node opens
// that session, mirrors what the radio says about itself, and exposes the one
// thing worth changing -- the channel.
//
// WHAT IT DELIBERATELY DOES NOT DO. It does not read the codeplug, it never
// keys the transmitter, and it sends no RF tuning command. See docs/xpr.md.
//
// Modes:
//   --probe    connect, print identity and channel, exit. The first thing to
//              run on a bench: it answers "is the link up and is it the radio
//              I think it is" without publishing anything.
//   (default)  run.

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "node_config.h"
#include "publishers.h"
#include "services.h"
#include "xpr_radio.capnp.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "xpr/radio.h"
#include "xpr/tcp_stream.h"

namespace
{

std::atomic<bool> gRunning { true };

void handleSignal(int)
{
    gRunning.store(false);
}

using namespace xpr_node;

xpr::Radio::StreamFactory tcpFactory(const RadioConfig& radio)
{
    return [host = radio.host, port = radio.port,
            timeout = std::chrono::milliseconds(radio.connectTimeoutMs)]()
               -> xpr::Result<std::unique_ptr<xpr::ByteStream>> {
        xpr::Result<std::unique_ptr<xpr::TcpStream>> stream = xpr::TcpStream::connect(host, port, timeout);
        if (!stream.has_value())
        {
            return std::unexpected(stream.error());
        }

        SPDLOG_INFO("xpr: connected to {}", (*stream)->peer());
        return std::unique_ptr<xpr::ByteStream>(std::move(*stream));
    };
}

xpr::Radio::Options radioOptions(const NodeConfig& config)
{
    xpr::Radio::Options options;
    options.replyTimeout = std::chrono::milliseconds(config.radio.replyTimeoutMs);

    options.reconnectBackoff.clear();
    for (const std::uint32_t ms : config.radio.reconnectBackoffMs)
    {
        options.reconnectBackoff.push_back(std::chrono::milliseconds(ms));
    }

    return options;
}

int runProbe(const NodeConfig& config)
{
    xpr::Radio radio(tcpFactory(config.radio), radioOptions(config));

    SPDLOG_INFO("xpr: probing {}:{}", config.radio.host, config.radio.port);

    if (const xpr::Result<void> connected = radio.connect(); !connected.has_value())
    {
        SPDLOG_ERROR("xpr: {}", xpr::to_string(connected.error()));
        return 1;
    }

    const xpr::Result<xpr::Radio::Identity> identity = radio.identity();
    if (identity.has_value())
    {
        SPDLOG_INFO("  model    {}", identity->modelNumber);
        SPDLOG_INFO("  serial   {}", identity->serialNumber);
        SPDLOG_INFO("  firmware {}", identity->firmwareVersion);
        SPDLOG_INFO("  tanapa   {}", identity->tanapaNumber);
        if (identity->radioIdKnown)
        {
            SPDLOG_INFO("  radio id {}", identity->radioId);
        }
    }
    else
    {
        SPDLOG_WARN("  identity: {}", xpr::to_string(identity.error()));
    }

    const xpr::Result<ChannelState> channel = read_channel(radio);
    if (channel.has_value())
    {
        SPDLOG_INFO("  zone {} of {}, channel {} of {}", channel->zone, channel->zoneCount,
                    channel->channel, channel->channelsInZone);
    }
    else
    {
        SPDLOG_WARN("  channel: {}", xpr::to_string(channel.error()));
    }

    return 0;
}

// Whether two channel readings say the same thing. The `fromBroadcast` flag is
// deliberately not compared: it records how we learned it, not what it is.
bool sameChannel(const ChannelState& a, const ChannelState& b)
{
    return a.zone == b.zone && a.channel == b.channel && a.zoneCount == b.zoneCount &&
           a.channelsInZone == b.channelsInZone;
}

void publishStatus(pub_sub::ZenohPublisher<::XprRadioStatus>& publisher, const NodeConfig& config,
                   const xpr::Radio::Stats& stats, const SharedState& state)
{
    ::XprRadioStatus::Builder out = publisher.fields();

    out.setConnected(stats.connected);
    out.setRadioHost(config.radio.host);
    out.setRadioPort(config.radio.port);
    out.setAddress(stats.address);

    out.setConnects(stats.connects);
    out.setConnectFailures(stats.connectFailures);
    out.setDisconnects(stats.disconnects);
    out.setLastError(stats.lastError);

    out.setCommands(stats.commands);
    out.setReplies(stats.replies);
    out.setBroadcasts(stats.broadcasts);
    out.setBroadcastsDropped(stats.broadcastsDropped);
    out.setFramesSkipped(stats.framesSkipped);
    out.setDecodeErrors(stats.decodeErrors);

    const std::optional<xpr::Radio::Identity> identity = state.identity();
    out.setIdentityKnown(identity.has_value());
    if (identity.has_value())
    {
        fillIdentity(out.initIdentity(), *identity);
    }

    const std::optional<ChannelState> channel = state.channel();
    out.setChannelKnown(channel.has_value());
    if (channel.has_value())
    {
        fillChannel(out.initChannel(), *channel);
    }

    out.setChannelControlEnabled(config.control.allowChannelChange);

    publisher.put();
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    std::string configPath;
    bool probe = false;
    bool debug = false;

    try
    {
        cxxopts::Options options("xpr_bridge", "Bridge a Motorola MOTOTRBO radio onto zenoh");
        options.add_options()
            ("c,config", "YAML configuration file", cxxopts::value<std::string>(configPath))
            ("probe", "Print the radio's identity and channel, then exit", cxxopts::value<bool>(probe))
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
        SPDLOG_ERROR("xpr: {}", e.what());
        return 2;
    }

    if (debug)
    {
        spdlog::set_level(spdlog::level::debug);
    }

    if (configPath.empty())
    {
        SPDLOG_ERROR("xpr: --config is required");
        return 2;
    }

    NodeConfig config;
    if (!load_node_config(configPath, config))
    {
        return 1;
    }

    if (probe)
    {
        return runProbe(config);
    }

    // Declared before any publisher, so a tool watching the bus sees the node
    // appear before its topics do.
    pub_sub::NodeIdentity identity("xpr");

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    xpr::Radio radio(tcpFactory(config.radio), radioOptions(config));
    SharedState state;
    Publishers publishers(config.publish);

    // Held by pointer so it can be torn down before the radio it calls into.
    auto services = std::make_unique<Services>(Services::Deps { &radio, &state, &config });

    pub_sub::ZenohPublisher<::XprRadioStatus> statusPublisher(config.publish.statusKey);

    SPDLOG_INFO("xpr: radio {}:{}, channel control {}", config.radio.host, config.radio.port,
                config.control.allowChannelChange ? "enabled" : "disabled");

    // The radio's own screen, as far as we have seen it. The radio pushes one
    // line at a time, so this is assembled here rather than in the message.
    std::array<std::string, kDisplayLines> displayLines;

    bool sessionUp = false;
    std::optional<ChannelState> published;
    auto nextStatus = std::chrono::steady_clock::now();

    while (gRunning.load())
    {
        // Collect whatever the radio has to say, and reconnect if it has
        // nothing because it went away.
        radio.pump(std::chrono::milliseconds(50));

        const bool connected = radio.connected();

        if (connected && !sessionUp)
        {
            sessionUp = true;

            if (const xpr::Result<xpr::Radio::Identity> read = radio.identity(); read.has_value())
            {
                state.setIdentity(*read);
                SPDLOG_INFO("xpr: {} serial {} firmware {}", read->modelNumber, read->serialNumber,
                            read->firmwareVersion);
            }

            if (const xpr::Result<ChannelState> read = read_channel(radio); read.has_value())
            {
                state.setChannel(*read);
            }
        }
        else if (!connected && sessionUp)
        {
            sessionUp = false;
            // A channel served from before the radio went away is worse than
            // none: it may have been turned while it was gone.
            state.clear();
            displayLines = {};
        }

        for (const xpr::Broadcast& broadcast : radio.takeBroadcasts())
        {
            if (broadcast.zoneChannel.has_value())
            {
                ChannelState channel;
                channel.zone = broadcast.zoneChannel->zone;
                channel.channel = broadcast.zoneChannel->channel;
                channel.fromBroadcast = true;

                // The broadcast does not carry the counts, so they come
                // forward from the last query. Without this a channel change
                // would report "3 of 0" until something asked again.
                if (const std::optional<ChannelState> previous = state.channel(); previous.has_value())
                {
                    channel.zoneCount = previous->zoneCount;
                    channel.channelsInZone = previous->channelsInZone;
                }

                state.setChannel(channel);
                continue;
            }

            if (broadcast.display.has_value())
            {
                // Lines are numbered from one.
                const std::uint8_t line = broadcast.display->line;
                if (line >= 1 && line <= kDisplayLines)
                {
                    displayLines[line - 1] = broadcast.display->text;
                }

                publishers.publishDisplay(line, displayLines);
                continue;
            }

            publishers.publishBroadcast(broadcast);
        }

        // ONE PLACE PUBLISHES THE CHANNEL, and it publishes whatever the
        // shared state says. A broadcast, the read after connect and a
        // set_channel service call all just write that state, so a change made
        // through the service reaches the topic even on a radio that does not
        // broadcast one -- and there is no path that updates the state and
        // forgets to say so.
        if (const std::optional<ChannelState> current = state.channel();
            current.has_value() && (!published.has_value() || !sameChannel(*current, *published)))
        {
            publishers.publishChannel(*current);
            published = current;
        }
        else if (!current.has_value())
        {
            published.reset();
        }

        if (const auto now = std::chrono::steady_clock::now(); now >= nextStatus)
        {
            publishStatus(statusPublisher, config, radio.stats(), state);
            nextStatus = now + std::chrono::milliseconds(config.publish.statusIntervalMs);
        }
    }

    SPDLOG_INFO("xpr: shutting down");

    // The services call into the radio, so they go first: a query still in
    // flight would otherwise reconnect the session we are closing.
    services.reset();
    radio.disconnect();

    const xpr::Radio::Stats stats = radio.stats();
    SPDLOG_INFO("xpr: {} command(s), {} broadcast(s), {} dropped, {} connect failure(s)", stats.commands,
                stats.broadcasts, stats.broadcastsDropped, stats.connectFailures);

    return 0;
}
