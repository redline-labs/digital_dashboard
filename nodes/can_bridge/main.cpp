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

#include "node_config.h"
#include "trc_recorder.h"

#include "can/backend.h"
#include "can/channel.h"
#include "can_backends/registry.h"

#include "can_bridge.capnp.h"
#include "can_frame.capnp.h"
#include "pub_sub/zenoh_client.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_service.h"
#include "pub_sub/zenoh_subscriber.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <memory>
#include <mutex>
#include <thread>

namespace
{

std::atomic<bool> running { true };

void handle_signal(int)
{
    running = false;
}

CanBusState to_schema_state(can::BusState state)
{
    switch (state)
    {
    case can::BusState::Unknown: return CanBusState::UNKNOWN;
    case can::BusState::ErrorActive: return CanBusState::ERROR_ACTIVE;
    case can::BusState::ErrorWarning: return CanBusState::ERROR_WARNING;
    case can::BusState::ErrorPassive: return CanBusState::ERROR_PASSIVE;
    case can::BusState::BusOff: return CanBusState::BUS_OFF;
    case can::BusState::Stopped: return CanBusState::STOPPED;
    }
    return CanBusState::UNKNOWN;
}

// One configured bus: the hardware, the topics, and the thread pumping between
// them.
class BridgedChannel
{
public:
    BridgedChannel(can_bridge::ChannelConfig config, std::shared_ptr<can::Channel> channel)
        : config_(std::move(config))
        , channel_(std::move(channel))
    {
        if (config_.publishRx)
        {
            rxPublisher_ = std::make_unique<pub_sub::ZenohPublisher<::CanFrame>>(config_.rxKey);
        }

        if (!config_.recordTrcPath.empty())
        {
            can::trc::BusInfo busInfo;
            busInfo.bus = config_.recordTrcBus;
            busInfo.name = config_.name;
            busInfo.connection = config_.device;
            busInfo.bitrateBps = config_.bitrateBps;
            busInfo.dataBitrateBps = config_.dataBitrateBps;

            auto recorder = can_bridge::TrcRecorder::create(config_.recordTrcPath, config_.recordTrcBus,
                                                busInfo);
            if (!recorder.has_value())
            {
                // Not fatal. A bridge that refused to carry traffic because a
                // log file could not be opened would be a bridge taken down by
                // a full disk.
                SPDLOG_ERROR("[{}] cannot record to '{}': {}", config_.name,
                             config_.recordTrcPath, recorder.error().message);
            }
            else
            {
                recorder_ = std::move(*recorder);
                SPDLOG_INFO("[{}] recording to '{}' as bus {}", config_.name,
                            config_.recordTrcPath, config_.recordTrcBus);
            }
        }
    }

    ~BridgedChannel() { stop(); }

    BridgedChannel(const BridgedChannel&) = delete;
    BridgedChannel& operator=(const BridgedChannel&) = delete;

    const can_bridge::ChannelConfig& config() const { return config_; }
    const std::shared_ptr<can::Channel>& channel() const { return channel_; }

    void start()
    {
        if (config_.acceptTx)
        {
            txSubscriber_ = std::make_unique<pub_sub::ZenohTypedSubscriber<::CanFrame>>(
                config_.txKey, [this](::CanFrame::Reader message) { transmit(message); });
        }

        // Recording needs the receive loop just as much as publishing does, so
        // a channel with publish_rx off still pumps when it is being recorded.
        // Without this a `publish_rx: false` channel would produce a trace
        // containing only the frames the node transmitted.
        if (config_.publishRx || recorder_)
        {
            pumping_ = true;
            pump_ = std::thread([this] { pump(); });
        }
    }

    void stop()
    {
        if (pump_.joinable())
        {
            pumping_ = false;
            pump_.join();
        }
        txSubscriber_.reset();
        // After both producers are gone, so the recorder's destructor drains a
        // queue nothing is still pushing to and the trace ends where the
        // traffic did.
        recorder_.reset();
    }

    // What this channel is doing, for the status topic.
    void fill_status(CanBridgeChannelStatus::Builder builder) const
    {
        builder.setName(config_.name);
        builder.setDevice(config_.device);
        builder.setDescription(channel_->description());
        builder.setOpen(true);
        builder.setRunning(channel_->running());
        builder.setListenOnly(channel_->listen_only());

        const auto bitrate = channel_->bitrate();
        builder.setNominalBps(bitrate.nominalBps);
        builder.setDataBps(bitrate.dataBps);

        const auto statistics = channel_->statistics();
        builder.setState(to_schema_state(statistics.state));
        builder.setRxFrames(statistics.rxFrames);
        builder.setTxFrames(statistics.txFrames);
        builder.setRxDropped(statistics.rxDropped);
        builder.setTxDropped(statistics.txDropped);
        builder.setErrorFrames(statistics.errorFrames);
        builder.setBusOffCount(statistics.busOffCount);
        builder.setRxErrorCounter(statistics.rxErrorCounter);
        builder.setTxErrorCounter(statistics.txErrorCounter);

        if (recorder_)
        {
            builder.setRecordPath(recorder_->path());
            builder.setRecordedFrames(recorder_->recorded());
            builder.setRecordDropped(recorder_->dropped());
        }

        std::lock_guard<std::mutex> lock(errorMutex_);
        builder.setError(lastError_);
    }

private:
    void transmit(::CanFrame::Reader message)
    {
        helpers::CanFrame frame {};
        frame.id = message.getId();
        frame.len = message.getLen();
        frame.isExtended = message.getExtended();
        frame.isRTR = message.getRtr();
        frame.isFD = message.getFd();
        frame.isBRS = message.getBrs();
        frame.isESI = message.getEsi();

        auto data = message.getData();
        const size_t n
            = std::min<size_t>(frame.data.size(), std::min<size_t>(frame.len, data.size()));
        for (size_t i = 0; i < n; ++i)
        {
            frame.data[i] = static_cast<uint8_t>(data[i]);
        }
        // A publisher that set `len` larger than the payload it supplied would
        // otherwise put uninitialised bytes on the bus.
        frame.len = static_cast<uint8_t>(n);

        auto result = channel_->send(frame);
        if (result.has_value() && recorder_)
        {
            // Only what actually reached the bus. Recording a frame the
            // adapter refused would put a message in the trace that was never
            // on the wire, which is the one thing a trace must not do.
            recorder_->record_tx(frame);
        }
        if (!result.has_value())
        {
            note_error(can::to_string(result.error()));
            // Rate-limited by the fact that a broken bus produces the same
            // message every time; logging every failure on a bus that is down
            // would drown everything else.
            SPDLOG_WARN("[{}] cannot transmit 0x{:X}: {}", config_.name, frame.id,
                        result.error().message);
        }
    }

    void pump()
    {
        // A batch, because a busy bus delivers faster than one frame per
        // wakeup and taking them one at a time turns a burst into a backlog.
        std::array<helpers::CanFrame, 64> batch;

        while (pumping_ && running)
        {
            auto count = channel_->receive(batch, can::Duration { 100 });
            if (!count.has_value())
            {
                note_error(can::to_string(count.error()));
                SPDLOG_WARN("[{}] receive failed: {}", config_.name, count.error().message);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            for (size_t i = 0; i < *count; ++i)
            {
                // Recorded before published, so the offset written to the trace
                // is as close to the wire as this process can make it -- a
                // zenoh put is not slow, but it is not free either.
                if (recorder_)
                {
                    recorder_->record_rx(batch[i]);
                }
                if (rxPublisher_)
                {
                    publish(batch[i]);
                }
            }
        }
    }

    void publish(const helpers::CanFrame& frame)
    {
        auto& fields = rxPublisher_->fields();
        fields.setId(frame.id);
        fields.setLen(frame.len);
        fields.setExtended(frame.isExtended);
        fields.setRtr(frame.isRTR);
        fields.setFd(frame.isFD);
        fields.setBrs(frame.isBRS);
        fields.setEsi(frame.isESI);
        fields.setError(frame.isError);
        fields.setTimestampUs(frame.timestampUs);
        fields.setChannel(config_.name);

        const size_t n = std::min<size_t>(frame.data.size(), frame.len);
        auto data = fields.initData(static_cast<unsigned>(n));
        for (size_t i = 0; i < n; ++i)
        {
            data.set(static_cast<unsigned>(i), frame.data[i]);
        }

        rxPublisher_->put();
    }

    void note_error(std::string message)
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = std::move(message);
    }

    can_bridge::ChannelConfig config_;
    std::shared_ptr<can::Channel> channel_;

    std::unique_ptr<pub_sub::ZenohPublisher<::CanFrame>> rxPublisher_;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<::CanFrame>> txSubscriber_;
    std::unique_ptr<can_bridge::TrcRecorder> recorder_;

    std::thread pump_;
    std::atomic<bool> pumping_ { false };

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

// A channel that could not be opened. Kept rather than dropped, so the status
// topic reports what is wrong with it instead of it simply being absent -- "the
// adapter is held by the kernel driver" is a far more useful thing to publish
// than nothing at all.
struct FailedChannel
{
    can_bridge::ChannelConfig config;
    std::string error;
};

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
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("can_bridge", "Bridge CAN hardware to zenoh topics");
    options.add_options()
        ("config", "Node configuration YAML", cxxopts::value<std::string>())
        ("l,list", "List the CAN channels this machine can see, then exit")
        // The client side of the bitrate service, so changing a running
        // bridge's bit rate does not need a program written for the occasion.
        ("set-bitrate",
         "Ask a running bridge to change a channel's bit rate, then exit: "
         "<channel>=<nominal>[:<data>]",
         cxxopts::value<std::string>())
        ("service-key", "Which bridge to ask, when --set-bitrate is used",
         cxxopts::value<std::string>()->default_value("vehicle/can/set_bitrate"))
        ("v,verbose", "Enable debug logging")
        ("h,help", "Print usage");

    cxxopts::ParseResult args;
    try
    {
        args = options.parse(argc, argv);
    }
    catch (const std::exception& error)
    {
        SPDLOG_ERROR("[node] {}", error.what());
        return 1;
    }

    if (args.count("help") != 0)
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }
    if (args.count("verbose") != 0)
    {
        spdlog::set_level(spdlog::level::debug);
    }

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

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    can::DefaultRegistryOptions registryOptions;
    registryOptions.pcan.detachKernelDriver = config.pcanDetachKernelDriver;
    registryOptions.trc.speed = config.trcReplaySpeed;
    registryOptions.trc.paced = config.trcReplayPaced;
    registryOptions.trc.loop = config.trcReplayLoop;
    auto registry = can::make_default_registry(registryOptions);

    // --- open what was asked for --------------------------------------------
    std::vector<std::unique_ptr<BridgedChannel>> channels;
    std::vector<FailedChannel> failed;

    for (const auto& channelConfig : config.channels)
    {
        can::OpenOptions open;
        open.bitrate.nominalBps = channelConfig.bitrateBps;
        open.bitrate.dataBps = channelConfig.dataBitrateBps;
        open.bitrate.nominalSamplePointPermille = channelConfig.samplePointPermille;
        open.bitrate.dataSamplePointPermille = channelConfig.dataSamplePointPermille;
        open.listenOnly = channelConfig.listenOnly;
        open.rxQueueDepth = channelConfig.rxQueueDepth;
        open.start = true;

        auto opened = registry.open(channelConfig.device, open);
        if (!opened.has_value())
        {
            SPDLOG_ERROR("[{}] cannot open {}: {}", channelConfig.name, channelConfig.device,
                         opened.error().message);
            failed.push_back(FailedChannel { channelConfig, opened.error().message });
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

        channels.push_back(std::make_unique<BridgedChannel>(channelConfig, *opened));
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

    auto publishStatus = [&]
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

        statusPublisher.put();
    };

    pub_sub::ZenohService<CanBridgeSetBitrateRequest, CanBridgeSetBitrateResponse> bitrateService(
        config.setBitrateKey,
        [&](const CanBridgeSetBitrateRequest::Reader& request,
            CanBridgeSetBitrateResponse::Builder& response)
        {
            const std::string name = request.getChannel();

            BridgedChannel* target = nullptr;
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
            publishStatus();
        });

    SPDLOG_INFO("[node] bridging {} channel(s); status on '{}', bitrate service on '{}'",
                channels.size(), config.statusKey, config.setBitrateKey);

    publishStatus();

    // --- run ----------------------------------------------------------------
    auto nextStatus = std::chrono::steady_clock::now();
    while (running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextStatus)
        {
            publishStatus();
            nextStatus = now + std::chrono::milliseconds(config.statusIntervalMs);
        }
    }

    // --- shutdown -----------------------------------------------------------
    //
    // Stop the pumps before the channels, so nothing is mid-receive when the
    // hardware goes away.
    SPDLOG_INFO("[node] shutting down");
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
