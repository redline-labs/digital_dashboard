#include "bridged_channel.h"

#include "pub_sub/can_frame.h"

#include <spdlog/spdlog.h>

#include <array>
#include <utility>

namespace can_bridge
{

namespace
{

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

}  // namespace

can::OpenOptions openOptions(const ChannelConfig& config)
{
    can::OpenOptions open;
    open.bitrate.nominalBps = config.bitrateBps;
    open.bitrate.dataBps = config.dataBitrateBps;
    open.bitrate.nominalSamplePointPermille = config.samplePointPermille;
    open.bitrate.dataSamplePointPermille = config.dataSamplePointPermille;
    open.listenOnly = config.listenOnly;
    open.rxQueueDepth = config.rxQueueDepth;
    open.start = true;
    return open;
}

BridgedChannel::BridgedChannel(ChannelConfig config, std::shared_ptr<can::Channel> channel)
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

        auto recorder = TrcRecorder::create(config_.recordTrcPath, config_.recordTrcBus, busInfo);
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

BridgedChannel::~BridgedChannel()
{
    stop();
}

std::shared_ptr<can::Channel> BridgedChannel::channel() const
{
    const std::lock_guard<std::mutex> lock(channelMutex_);
    return channel_;
}

void BridgedChannel::start()
{
    if (config_.acceptTx)
    {
        txSubscriber_ = std::make_unique<pub_sub::ZenohTypedSubscriber<::CanFrame>>(
            config_.txKey, [this](::CanFrame::Reader message) { transmit(message); });
    }
    startPump();
    started_ = true;
}

void BridgedChannel::stop()
{
    started_ = false;
    stopPump();
    txSubscriber_.reset();
    // After both producers are gone, so the recorder's destructor drains a
    // queue nothing is still pushing to and the trace ends where the
    // traffic did.
    recorder_.reset();
}

bool BridgedChannel::lost() const
{
    return started_ && (deviceGone_ || !channel()->running());
}

void BridgedChannel::reattach(std::shared_ptr<can::Channel> channel)
{
    stopPump();
    {
        const std::lock_guard<std::mutex> lock(channelMutex_);
        channel_ = std::move(channel);
    }
    deviceGone_ = false;
    note_error("");
    reopen_.reset();
    startPump();
}

void BridgedChannel::startPump()
{
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

void BridgedChannel::stopPump()
{
    if (pump_.joinable())
    {
        pumping_ = false;
        pump_.join();
    }
}

void BridgedChannel::fill_status(CanBridgeChannelStatus::Builder builder) const
{
    const std::shared_ptr<can::Channel> channel = this->channel();
    builder.setName(config_.name);
    builder.setDevice(config_.device);
    builder.setDescription(channel->description());
    builder.setOpen(!deviceGone_);
    builder.setRunning(channel->running() && !deviceGone_);
    builder.setListenOnly(channel->listen_only());

    const auto bitrate = channel->bitrate();
    builder.setNominalBps(bitrate.nominalBps);
    builder.setDataBps(bitrate.dataBps);

    const auto statistics = channel->statistics();
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

void BridgedChannel::transmit(::CanFrame::Reader message)
{
    // fromCapnp caps `len` at the payload actually supplied. A publisher
    // that set `len` larger would otherwise put uninitialised bytes on the
    // bus.
    const helpers::CanFrame frame = pub_sub::fromCapnp(message);

    auto result = channel()->send(frame);
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

void BridgedChannel::pump()
{
    // A batch, because a busy bus delivers faster than one frame per
    // wakeup and taking them one at a time turns a burst into a backlog.
    std::array<helpers::CanFrame, 64> batch;
    const std::shared_ptr<can::Channel> channel = this->channel();

    while (pumping_)
    {
        auto count = channel->receive(batch, can::Duration { 100 });
        if (!count.has_value())
        {
            note_error(can::to_string(count.error()));
            if (count.error().kind == can::Error::Kind::NotFound)
            {
                // The adapter is gone. Said once, and the loop ends: the node
                // reopens the channel, which is what brings it back.
                SPDLOG_ERROR("[{}] {}; will reopen {}", config_.name, count.error().message,
                             config_.device);
                deviceGone_ = true;
                return;
            }
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

void BridgedChannel::publish(const helpers::CanFrame& frame)
{
    auto& fields = rxPublisher_->fields();
    pub_sub::toCapnp(frame, fields);
    fields.setChannel(config_.name);
    rxPublisher_->put();
}

void BridgedChannel::note_error(std::string message)
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = std::move(message);
}

}  // namespace can_bridge
