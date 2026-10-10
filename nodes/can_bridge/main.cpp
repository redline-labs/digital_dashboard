// SPDX-License-Identifier: GPL-3.0-or-later
//
// can_bridge -- CAN hardware on one side, zenoh topics on the other.
//
// This is the node the repository did not have: every other CAN node here is
// receive-only, and nothing terminated `vehicle/can0/tx` at all. With this
// running, anything that publishes to a tx key reaches the wire, which is what
// the CANopen reconfiguration tool and anything else that has to talk rather
// than listen has been waiting for.
//
// A log file is a bus here too. `device: "trc:run.trc"` opens a recorded PCAN
// trace as a channel and replays it at its recorded timing, which is what the
// separate `can_replay` node used to do -- badly, since its parser discarded
// every timestamp it read. `record_trc:` on any channel is the same thing in
// reverse, writing a trace PCAN-Explorer can open.
//
// Shape: one thread per channel doing a blocking receive and publishing what it
// gets; zenoh subscriber callbacks calling send() directly. That is why
// can::Channel promises send() is safe while another thread is in receive() --
// this is the caller that needs it, and nothing needs more.
//
// A channel that fails to open does not take the others down. Two buses on one
// vehicle should not both stop because one adapter was unplugged, and a bridge
// that exits on the first problem is a bridge that has to be babysat.

#include "cli/interrupt.h"
#include "cli/node_options.h"
#include "bridged_channel.h"
#include "channel_health.h"
#include "node_config.h"
#include "trc_recorder.h"

#include "can/backend.h"
#include "can/channel.h"
#include "can_backends/registry.h"

#include "can_bridge.capnp.h"
#include "can_frame.capnp.h"
#include "node_health/reporter.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/zenoh_client.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_service.h"
#include "pub_sub/zenoh_subscriber.h"

#include <cxxopts.hpp>
#include "core/core.h"
#include <spdlog/spdlog.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace
{



// What a --set-bitrate argument asks for.
struct BitrateRequest
{
    std::string channel;
    uint32_t nominalBps { 0 };
    uint32_t dataBps { 0 };
};

// "<channel>=<nominal>[:<data>]", e.g. "chassis=250000" or "can0=500000:2000000".
std::optional<BitrateRequest> parse_set_bitrate(const std::string& text)
{
    const size_t equals = text.find('=');
    if (equals == std::string::npos || equals == 0)
    {
        SPDLOG_ERROR("[node] --set-bitrate wants <channel>=<nominal>[:<data>], for example "
                     "'chassis=250000' or 'can0=500000:2000000'");
        return std::nullopt;
    }

    BitrateRequest request;
    request.channel = text.substr(0, equals);

    std::string rates = text.substr(equals + 1);
    const size_t colon = rates.find(':');
    std::string nominal = rates;
    std::string data;
    if (colon != std::string::npos)
    {
        nominal = rates.substr(0, colon);
        data = rates.substr(colon + 1);
    }

    try
    {
        request.nominalBps = static_cast<uint32_t>(std::stoul(nominal));
        if (!data.empty())
        {
            request.dataBps = static_cast<uint32_t>(std::stoul(data));
        }
    }
    catch (const std::exception&)
    {
        SPDLOG_ERROR("[node] '{}' is not a bit rate", rates);
        return std::nullopt;
    }

    if (request.nominalBps == 0)
    {
        SPDLOG_ERROR("[node] a bit rate of 0 is not a bit rate");
        return std::nullopt;
    }

    return request;
}

bool call_set_bitrate(const std::string& key, const BitrateRequest& request)
{
    pub_sub::ZenohClient<CanBridgeSetBitrateRequest, CanBridgeSetBitrateResponse> client(key, 2000);

    auto& fields = client.fields();
    fields.setChannel(request.channel);
    fields.setNominalBps(request.nominalBps);
    fields.setDataBps(request.dataBps);

    bool ok = false;
    // False means nobody answered, which for a service is how you discover the
    // node providing it is not running -- zenoh has no registry to ask.
    const bool answered = client.request(
        [&](CanBridgeSetBitrateResponse::Reader response)
        {
            ok = response.getOk();
            if (ok)
            {
                SPDLOG_INFO("[node] {} is now at {} bit/s{}", request.channel,
                            response.getActualNominalBps(),
                            response.getActualDataBps() != 0
                                ? fmt::format(" + {} bit/s data", response.getActualDataBps())
                                : "");
            }
            else
            {
                SPDLOG_ERROR("[node] {}", response.getError().cStr());
                // What the channel was left at matters as much as the failure:
                // it says whether the bus is still usable. Only meaningful when
                // there was a channel -- a request naming one that does not
                // exist has nothing to report.
                if (response.getActualNominalBps() != 0)
                {
                    SPDLOG_ERROR("[node] {} is still at {} bit/s", request.channel,
                                 response.getActualNominalBps());
                }
            }
        });

    if (!answered)
    {
        SPDLOG_ERROR("[node] no bridge answered on '{}' -- is one running?", key);
    }

    return answered && ok;
}

// Two backends never appear here, and cannot: `virtual:` and `trc:` exist only
// once something names one, so there is nothing to enumerate. Saying so is more
// useful than an empty list, which reads as "this machine cannot do CAN".
void print_no_hardware_options()
{
    SPDLOG_INFO("a virtual bus is always available as 'virtual:<name>' -- it needs no "
                "hardware and is how this node is exercised without an adapter");
    SPDLOG_INFO("so is a recorded trace, as 'trc:<path.trc>[/<bus>]' -- it replays a PCAN "
                ".trc file at its recorded timing; see configs/can_bridge/replay.yaml");
}

void print_channel_list(const can::Registry& registry)
{
    auto found = registry.enumerate();
    if (found.empty())
    {
        SPDLOG_INFO("no CAN channels found");
        print_no_hardware_options();
        return;
    }

    SPDLOG_INFO("{} CAN channel(s):", found.size());
    for (const auto& info : found)
    {
        if (info.available)
        {
            SPDLOG_INFO("  {:<24} {}{}{}", info.id.toString(), info.description,
                        info.supportsFd ? " [CAN FD]" : "",
                        // The index in the id shifts when adapters are
                        // unplugged; the serial does not, and 'pcan:<serial>'
                        // is accepted anywhere the index is.
                        info.serial.empty() ? "" : fmt::format(" [serial {}]", info.serial));
        }
        else
        {
            SPDLOG_INFO("  {:<24} {} -- UNAVAILABLE: {}", info.id.toString(), info.description,
                        info.unavailableReason);
        }
    }
    print_no_hardware_options();
}

} // namespace

int main(int argc, char** argv)
{
    core::setupLogging({.program = "can_bridge"});

    cli::NodeCommandLine cli("can_bridge", "Bridge CAN hardware to zenoh topics");
    cli.withConfig();
    cli.add()
        ("l,list", "List the CAN channels this machine can see, then exit")
        ("set-bitrate",
         "Ask a running bridge to change a channel's bit rate, then exit: "
         "<channel>=<nominal>[:<data>]",
         cxxopts::value<std::string>())
        ("service-key", "Which bridge to ask, when --set-bitrate is used",
         cxxopts::value<std::string>()->default_value("vehicle/can/set_bitrate"));
    if (const std::optional<int> exit = cli.parse(argc, argv))
    {
        return *exit;
    }
    const cxxopts::ParseResult& args = cli.result();

    // --list before --config, so "what can I even open" needs no config file.
    if (args.count("list") != 0)
    {
        auto registry = can::make_default_registry();
        print_channel_list(registry);
        return 0;
    }

    // Client mode: ask a bridge that is already running to retime a channel.
    if (args.count("set-bitrate") != 0)
    {
        auto request = parse_set_bitrate(args["set-bitrate"].as<std::string>());
        if (!request.has_value())
        {
            return 1;
        }
        return call_set_bitrate(args["service-key"].as<std::string>(), *request) ? 0 : 1;
    }

    if (args.count("config") == 0)
    {
        SPDLOG_ERROR("[node] --config is required. Start from "
                     "configs/can_bridge/can_bridge.yaml, which documents every field.");
        SPDLOG_ERROR("[node] --list shows what is attached without needing one.");
        return 1;
    }

    can_bridge::NodeConfig config;
    if (!can_bridge::load_node_config(args["config"].as<std::string>(), config))
    {
        SPDLOG_ERROR("[node] refusing to start with an unusable --config");
        return 1;
    }

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. See
    // pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("can_bridge");

    // One health topic per node, whatever it does: see libs/node_health.
    node_health::HealthReporter health("can_bridge");

    // Early, so a SIGTERM during setup still ends in an orderly shutdown and a
    // final "stopping" health sample rather than the default action.
    cli::installInterruptHandler();


    can::DefaultRegistryOptions registryOptions;
    registryOptions.pcan.detachKernelDriver = config.pcanDetachKernelDriver;
    registryOptions.trc.speed = config.trcReplaySpeed;
    registryOptions.trc.paced = config.trcReplayPaced;
    registryOptions.trc.loop = config.trcReplayLoop;
    auto registry = can::make_default_registry(registryOptions);

    // --- open what was asked for --------------------------------------------
    std::vector<std::unique_ptr<can_bridge::BridgedChannel>> channels;
    std::vector<can_bridge::FailedChannel> failed;

    for (const auto& channelConfig : config.channels)
    {
        auto opened = registry.open(channelConfig.device, can_bridge::openOptions(channelConfig));
        if (!opened.has_value())
        {
            SPDLOG_ERROR("[{}] cannot open {}: {}", channelConfig.name, channelConfig.device,
                         opened.error().message);
            failed.push_back(can_bridge::FailedChannel { channelConfig, opened.error().message, {} });
            failed.back().reopen.failed(std::chrono::steady_clock::now());
            if (!config.continueOnChannelError)
            {
                return 1;
            }
            continue;
        }

        SPDLOG_INFO("[{}] {} at {}{}", channelConfig.name, (*opened)->description(),
                    (*opened)->bitrate().toString(),
                    channelConfig.listenOnly ? ", listen-only" : "");
        SPDLOG_INFO("[{}]   rx -> '{}'{}", channelConfig.name, channelConfig.rxKey,
                    channelConfig.publishRx ? "" : " (not published)");
        SPDLOG_INFO("[{}]   tx <- '{}'{}", channelConfig.name, channelConfig.txKey,
                    channelConfig.acceptTx ? "" : " (not accepted)");

        channels.push_back(std::make_unique<can_bridge::BridgedChannel>(channelConfig, *opened));
    }

    if (channels.empty())
    {
        SPDLOG_ERROR("[node] no channel could be opened; there is nothing to bridge");
        return 1;
    }

    for (auto& channel : channels)
    {
        channel->start();
    }

    // --- status and control -------------------------------------------------
    pub_sub::ZenohPublisher<CanBridgeStatus> statusPublisher(config.statusKey);

    // Per channel, the drop counters as of the last status: see below.
    std::map<std::string, std::uint64_t> droppedSeen;

    // The main loop and the bit-rate service (a zenoh thread) share the channel
    // lists, the status builder, droppedSeen and the health checks. Everything
    // that touches them holds this.
    std::mutex stateMutex;

    auto publishStatusLocked = [&]
    {
        auto& fields = statusPublisher.fields();
        auto list = fields.initChannels(
            static_cast<unsigned>(channels.size() + failed.size()));

        unsigned index = 0;
        for (const auto& channel : channels)
        {
            channel->fill_status(list[index++]);
        }
        // The ones that did not open are reported too, with why.
        for (const auto& failure : failed)
        {
            auto entry = list[index++];
            entry.setName(failure.config.name);
            entry.setDevice(failure.config.device);
            entry.setOpen(false);
            entry.setRunning(false);
            entry.setError(failure.error);
            entry.setState(CanBusState::UNKNOWN);
        }

        // Health from the same numbers, before put() re-roots the builder.
        for (const auto channel : fields.asReader().getChannels())
        {
            const std::string name = "channel:" + std::string(channel.getName());
            const std::uint64_t dropped = channel.getRxDropped() + channel.getTxDropped();
            std::uint64_t& seen = droppedSeen[name];
            const bool dropping = dropped > seen;
            seen = dropped;

            const can_bridge::ChannelHealth check = can_bridge::channelHealth(channel, dropping);
            health.setCheck(name, check.state, check.reason);
        }

        statusPublisher.put();
    };

    // Held by pointer so shutdown can retire it before the channels it reaches.
    auto bitrateService = std::make_unique<
        pub_sub::ZenohService<CanBridgeSetBitrateRequest, CanBridgeSetBitrateResponse>>(
        config.setBitrateKey,
        [&](const CanBridgeSetBitrateRequest::Reader& request,
            CanBridgeSetBitrateResponse::Builder& response)
        {
            const std::lock_guard<std::mutex> lock(stateMutex);
            const std::string name = request.getChannel();

            can_bridge::BridgedChannel* target = nullptr;
            for (auto& channel : channels)
            {
                if (channel->config().name == name)
                {
                    target = channel.get();
                    break;
                }
            }

            if (target == nullptr)
            {
                std::string known;
                for (const auto& channel : channels)
                {
                    known += (known.empty() ? "" : ", ") + channel->config().name;
                }
                const std::string error = fmt::format(
                    "no channel named '{}'; this bridge has {}", name,
                    known.empty() ? "none open" : known);
                SPDLOG_WARN("[node] {}", error);
                response.setOk(false);
                response.setError(error);
                return;
            }

            can::Bitrate bitrate;
            bitrate.nominalBps = request.getNominalBps();
            bitrate.dataBps = request.getDataBps();
            bitrate.nominalSamplePointPermille = request.getNominalSamplePointPermille();
            bitrate.dataSamplePointPermille = request.getDataSamplePointPermille();

            SPDLOG_INFO("[{}] changing bit rate to {}", name, bitrate.toString());
            auto result = target->channel()->set_bitrate(bitrate);

            const auto actual = target->channel()->bitrate();
            response.setActualNominalBps(actual.nominalBps);
            response.setActualDataBps(actual.dataBps);
            response.setActualNominalSamplePointPermille(actual.nominalSamplePointPermille);

            if (!result.has_value())
            {
                SPDLOG_WARN("[{}] {}", name, result.error().message);
                response.setOk(false);
                response.setError(result.error().message);
                return;
            }

            SPDLOG_INFO("[{}] now at {}", name, actual.toString());
            response.setOk(true);
            response.setError("");
            publishStatusLocked();
        });

    // An adapter unplugged at runtime, or absent at startup, is opened again
    // when it comes back. Without this a replugged dongle stayed dark until
    // someone restarted the node: the process was up, so systemd never did.
    auto reopenLocked = [&](std::chrono::steady_clock::time_point now)
    {
        for (auto& channel : channels)
        {
            if (!channel->lost() || !channel->reopen().due(now))
            {
                continue;
            }
            auto opened = registry.open(channel->config().device,
                                        can_bridge::openOptions(channel->config()));
            if (!opened.has_value())
            {
                SPDLOG_DEBUG("[{}] still cannot open {}: {}", channel->config().name,
                             channel->config().device, opened.error().message);
                channel->reopen().failed(now);
                continue;
            }
            SPDLOG_INFO("[{}] reopened {}", channel->config().name, channel->config().device);
            channel->reattach(*opened);
        }

        for (auto it = failed.begin(); it != failed.end();)
        {
            if (!it->reopen.due(now))
            {
                ++it;
                continue;
            }
            auto opened = registry.open(it->config.device, can_bridge::openOptions(it->config));
            if (!opened.has_value())
            {
                it->error = opened.error().message;
                it->reopen.failed(now);
                ++it;
                continue;
            }
            SPDLOG_INFO("[{}] {} opened at last", it->config.name, it->config.device);
            auto bridged = std::make_unique<can_bridge::BridgedChannel>(it->config, *opened);
            bridged->start();
            channels.push_back(std::move(bridged));
            it = failed.erase(it);
        }
    };

    SPDLOG_INFO("[node] bridging {} channel(s); status on '{}', bitrate service on '{}'",
                channels.size(), config.statusKey, config.setBitrateKey);

    {
        const std::lock_guard<std::mutex> lock(stateMutex);
        publishStatusLocked();
    }

    // --- run ----------------------------------------------------------------
    auto nextStatus = std::chrono::steady_clock::now();
    health.markReady();

    cli::waitForInterrupt([&] {
        health.kick();
        const auto now = std::chrono::steady_clock::now();
        const std::lock_guard<std::mutex> lock(stateMutex);
        reopenLocked(now);
        if (now >= nextStatus)
        {
            publishStatusLocked();
            nextStatus = now + std::chrono::milliseconds(config.statusIntervalMs);
        }
    }, std::chrono::milliseconds(50));

    // --- shutdown -----------------------------------------------------------
    //
    // Stop the pumps before the channels, so nothing is mid-receive when the
    // hardware goes away.
    SPDLOG_INFO("[node] shutting down");
    // The service first, so no call is mid-way through a channel below.
    bitrateService.reset();
    for (auto& channel : channels)
    {
        channel->stop();
    }
    for (auto& channel : channels)
    {
        auto result = channel->channel()->stop();
        if (!result.has_value())
        {
            SPDLOG_DEBUG("[{}] {}", channel->config().name, result.error().message);
        }
    }

    return 0;
}
