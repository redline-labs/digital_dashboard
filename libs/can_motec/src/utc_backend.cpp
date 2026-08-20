// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_motec/utc_backend.h"

#include "utc_transport.h"

#include "can_motec/motec_gw.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <string_view>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace can::motec
{
namespace
{

// The receive path is one thread doing a blocking read and pushing what it
// gets into a queue, exactly as the PCAN backend does. What differs is that
// this device also answers commands on the same stream, so the reader has to
// separate the two: an Rx reply carries bus traffic, and everything else is
// somebody's answer.
class MotecChannel : public Channel
{
public:
    MotecChannel(ChannelId id, std::shared_ptr<Transport> transport, const OpenOptions& options,
                 const MotecOptions& motecOptions, uint8_t busHandle)
        : id_(std::move(id))
        , transport_(std::move(transport))
        , options_(motecOptions)
        , bitrate_(options.bitrate)
        , queueDepth_(options.rxQueueDepth)
        , busHandle_(busHandle)
    {
    }

    ~MotecChannel() override { stop_reader(); }

    const ChannelId& id() const override { return id_; }
    const std::string& description() const override { return transport_->description(); }

    Result<void> set_bitrate(const Bitrate& bitrate) override
    {
        // Refusing rather than accepting-and-ignoring. The register map behind
        // the Set command is unknown, so the only honest answers are this one
        // and a guess that silently leaves the bus at the wrong speed -- which
        // presents as a dead bus with no error anywhere.
        return unsupported(fmt::format(
            "a MoTeC UTC's bit rate cannot be set over this protocol -- the Set command's "
            "register map is not known. The device runs at whatever MoTeC's own tool last "
            "configured it for; set it to {} there. See docs/motec_utc.md",
            bitrate.toString()));
    }

    Bitrate bitrate() const override { return bitrate_; }

    bool supports_fd() const override { return false; }

    Result<void> set_listen_only(bool listenOnly) override
    {
        if (!listenOnly)
        {
            // Already the only mode there is.
            return {};
        }
        return unsupported("a MoTeC UTC has no listen-only mode in this protocol. A receive "
                           "filter is not equivalent: it stops frames being delivered, but the "
                           "controller still acknowledges them on the bus");
    }

    bool listen_only() const override { return false; }

    Result<void> start() override
    {
        if (running_)
        {
            return {};
        }

        // One subscribe, ever. The device then pushes data frames on its own
        // request-id counter roughly every 255 ms and never asks for anything
        // back -- so a driver that polled here would be talking over it.
        auto subscribed = transport_->send(encode_frame(make_rx_subscribe(busHandle_, next_reqid())));
        if (!subscribed.has_value())
        {
            return subscribed;
        }

        lastRxAt_ = std::chrono::steady_clock::now();
        running_ = true;
        pumping_ = true;
        reader_ = std::thread([this] { pump(); });
        return {};
    }

    Result<void> stop() override
    {
        stop_reader();
        running_ = false;
        return {};
    }

    bool running() const override { return running_; }

    Result<void> send(const helpers::CanFrame& frame) override
    {
        auto record = from_can_frame(frame);
        if (!record.has_value())
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            statistics_.txDropped++;
            return std::unexpected(record.error());
        }

        const std::array<Record, 1> records { *record };
        auto sent = transport_->send(encode_frame(make_tx(busHandle_, next_reqid(), records)));
        if (!sent.has_value())
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            statistics_.txDropped++;
            return sent;
        }

        // Counted as transmitted here rather than when the acknowledgement
        // arrives, because the caller's send() has to return without waiting
        // for a USB round trip. The reader corrects the count if the device
        // says it accepted fewer bytes than were offered.
        std::lock_guard<std::mutex> lock(statsMutex_);
        statistics_.txFrames++;
        statistics_.txBytes += frame.len;
        return {};
    }

    Result<size_t> receive(std::span<helpers::CanFrame> out, Duration timeout) override
    {
        if (out.empty())
        {
            return size_t { 0 };
        }

        std::unique_lock<std::mutex> lock(queueMutex_);
        if (queue_.empty())
        {
            queueReady_.wait_for(lock, timeout, [this] { return !queue_.empty() || !pumping_; });
        }

        size_t count = 0;
        while (count < out.size() && !queue_.empty())
        {
            out[count++] = queue_.front();
            queue_.pop_front();
        }
        return count;
    }

    Statistics statistics() const override
    {
        Statistics copy;
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            copy = statistics_;
        }

        if (!running_)
        {
            copy.state = BusState::Stopped;
            return copy;
        }

        // The protocol carries no controller state and no error counters, so
        // the strongest thing available is whether the device is still
        // talking. Reporting ErrorActive on a link that has gone silent would
        // be a guess in the flattering direction.
        const auto since = std::chrono::steady_clock::now() - lastRxAt_.load();
        const bool stalled = since > std::chrono::milliseconds(options_.rxStallTimeoutMs);
        copy.state = stalled ? BusState::Unknown : BusState::ErrorActive;
        return copy;
    }

    // Runs the Open/Version/Filter exchange and returns the bus handle.
    static Result<uint8_t> handshake(Transport& transport, const MotecOptions& options);

private:
    uint8_t next_reqid() { return static_cast<uint8_t>(nextReqid_++); }

    void stop_reader()
    {
        if (reader_.joinable())
        {
            pumping_ = false;
            queueReady_.notify_all();
            reader_.join();
        }
    }

    void pump()
    {
        std::vector<Frame> frames;
        auto nextKeepAlive = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(options_.keepAliveIntervalMs);

        while (pumping_)
        {
            // Sent from this thread rather than a timer of its own: it has to
            // keep going for as long as the stream is being read, and tying it
            // to the reader means there is no way to have one without the
            // other. See keepAliveIntervalMs -- without this the device stops
            // sending after about ten seconds.
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextKeepAlive)
            {
                nextKeepAlive = now + std::chrono::milliseconds(options_.keepAliveIntervalMs);
                auto sent = transport_->send(encode_frame(make_version(busHandle_, next_reqid())));
                if (!sent.has_value())
                {
                    SPDLOG_WARN("[motec] {} keep-alive failed: {}", id_.toString(),
                                sent.error().message);
                }
            }

            frames.clear();
            auto got = transport_->receive(frames, Duration { options_.readTimeoutMs });
            if (!got.has_value())
            {
                SPDLOG_WARN("[motec] {} receive failed: {}", id_.toString(), got.error().message);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            for (const auto& frame : frames)
            {
                dispatch(frame);
            }
        }
    }

    void dispatch(const Frame& frame)
    {
        switch (frame.code())
        {
        case Code::Rx:
        {
            const uint8_t status = frame.status().value_or(0);
            if (status != 0)
            {
                // The device refusing on the stream itself. 0x04 is what a
                // session that has timed out answers, and it used to be
                // dropped on the floor along with the frame.
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastStreamStatus_ = status;
                SPDLOG_WARN("[motec] {} receive stream reported status 0x{:02X}", id_.toString(),
                            status);
                break;
            }
            lastRxAt_ = std::chrono::steady_clock::now();
            deliver(frame);
            break;
        }

        case Code::Tx:
        {
            if (!frame.isReply())
            {
                break;
            }

            // Two ways a transmit fails, and both have been seen on hardware.
            //
            // A non-zero status is an outright refusal: 0x20 is the device
            // saying its transmit buffer is full. That happens transiently
            // under load, and permanently if nothing on the bus acknowledges --
            // a UTC alone on a bus accepts about four frames and then refuses
            // everything, because the controller is still retrying the first.
            //
            // A status of zero with a short byte count is the quieter one. Each
            // send() offers exactly one record, so anything other than a whole
            // record acknowledged means the frame did not go, and the previous
            // check for a whole multiple of the record size let a count of ZERO
            // through -- which is the number that matters.
            const uint8_t status = frame.status().value_or(0);
            const bool haveCount = frame.payload.size() >= 3;
            const uint16_t accepted = haveCount
                ? static_cast<uint16_t>((frame.payload[1] << 8) | frame.payload[2])
                : 0;

            const bool refused = status != 0;
            const bool shortCount = !refused && haveCount && accepted != kRecordSize;
            if (!refused && !shortCount)
            {
                break;
            }

            if (shortCount)
            {
                SPDLOG_WARN("[motec] {} transmit acknowledged {} of {} bytes", id_.toString(),
                            accepted, kRecordSize);
            }

            std::lock_guard<std::mutex> lock(statsMutex_);
            statistics_.txDropped++;
            // send() counted the frame optimistically, because it cannot wait
            // for a USB round trip. This is where that is put right.
            if (statistics_.txFrames != 0)
            {
                statistics_.txFrames--;
            }
            break;
        }

        case Code::Version:
            // The keep-alive's own reply. Expected, and says the device is
            // still answering, so it is not worth a line each time.
            break;

        case Code::Open:
        case Code::Poll:
        case Code::Ack:
        case Code::RegRead:
        case Code::Filter:
        case Code::Set:
            // Nothing is outstanding once the handshake is done, so anything
            // here is the device volunteering something. Logged rather than
            // dropped silently -- it is the sort of thing that would explain a
            // later failure.
            SPDLOG_DEBUG("[motec] {} unsolicited {} frame (reqid {})", id_.toString(),
                         to_string(frame.code()), frame.reqid);
            break;
        }
    }

    void deliver(const Frame& frame)
    {
        const auto records = unpack_records(frame.data);
        if (records.empty())
        {
            // The idle keep-alive. It is what tells us the link is alive, and
            // lastRxAt_ has already been stamped.
            return;
        }

        std::lock_guard<std::mutex> lock(queueMutex_);
        for (const auto& record : records)
        {
            if (queue_.size() >= queueDepth_)
            {
                // Oldest first: on a bus being logged, the most recent frames
                // are the ones worth keeping.
                queue_.pop_front();
                std::lock_guard<std::mutex> stats(statsMutex_);
                statistics_.rxDropped++;
            }
            queue_.push_back(to_can_frame(record));

            std::lock_guard<std::mutex> stats(statsMutex_);
            statistics_.rxFrames++;
            statistics_.rxBytes += std::min<uint8_t>(record.dlc(), 8);
        }
        queueReady_.notify_all();
    }

    ChannelId id_;
    std::shared_ptr<Transport> transport_;
    MotecOptions options_;
    Bitrate bitrate_;
    size_t queueDepth_ { 8192 };
    uint8_t busHandle_ { 1 };

    std::atomic<uint8_t> nextReqid_ { 1 };
    std::atomic<bool> running_ { false };
    std::atomic<bool> pumping_ { false };
    std::thread reader_;

    std::atomic<std::chrono::steady_clock::time_point> lastRxAt_ {
        std::chrono::steady_clock::time_point {}
    };

    mutable std::mutex queueMutex_;
    std::condition_variable queueReady_;
    std::deque<helpers::CanFrame> queue_;

    mutable std::mutex errorMutex_;
    uint8_t lastStreamStatus_ { 0 };

    mutable std::mutex statsMutex_;
    Statistics statistics_ {};
};

// Waits for the reply to `reqid`. Frames that are not it -- an Rx push that
// arrived early, say -- are dropped: nothing is subscribed yet during the
// handshake, so there is nothing they could belong to.
// Returns the reply whatever its status; the caller decides what a refusal
// means. Only the unlock path cares, but folding the status into an error
// message here would hide the one byte it needs.
Result<Frame> await_any_reply(Transport& transport, Code expected, uint8_t reqid,
                              unsigned int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    std::vector<Frame> frames;
    while (std::chrono::steady_clock::now() < deadline)
    {
        frames.clear();
        auto got = transport.receive(frames, Duration { 50 });
        if (!got.has_value())
        {
            return std::unexpected(got.error());
        }

        for (auto& frame : frames)
        {
            if (!frame.isReply() || frame.code() != expected)
            {
                continue;
            }
            if (frame.reqid != reqid)
            {
                // A reply to something else entirely. Matching on the request
                // id is the only thing that keeps two commands in flight from
                // being confused for one another.
                SPDLOG_DEBUG("[motec] ignoring a {} reply for request {} while waiting for {}",
                             to_string(expected), frame.reqid, reqid);
                continue;
            }

            return frame;
        }
    }

    return io_error(fmt::format("the gateway did not answer {} within {} ms", to_string(expected),
                                timeoutMs));
}

// The common case: a refusal is an error.
Result<Frame> await_reply(Transport& transport, Code expected, uint8_t reqid,
                          unsigned int timeoutMs)
{
    auto reply = await_any_reply(transport, expected, reqid, timeoutMs);
    if (!reply.has_value())
    {
        return reply;
    }
    const uint8_t status = reply->status().value_or(0xFF);
    if (status != 0)
    {
        return protocol_error(
            fmt::format("the gateway refused {} with status 0x{:02X}", to_string(expected), status));
    }
    return reply;
}

Result<uint8_t> MotecChannel::handshake(Transport& transport, const MotecOptions& options)
{
    uint8_t reqid = 1;

    // Open first: everything after it has to quote the bus handle it returns.
    auto sent = transport.send(encode_frame(make_open(reqid)));
    if (!sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto opened = await_any_reply(transport, Code::Open, reqid, options.handshakeTimeoutMs);
    if (!opened.has_value())
    {
        return std::unexpected(opened.error());
    }

    // A device that answers 0x21 has latched, and will answer 0x21 to
    // everything on this tag including further Opens. An Open on the
    // management tag clears it. Done here rather than left to a human because
    // the alternative is unplugging the dongle: nothing short of removing its
    // power gets it back otherwise, and a node that crashed at the wrong
    // moment would stay dead until someone walked to the vehicle.
    if (opened->status().value_or(0) == kStatusLatched)
    {
        SPDLOG_WARN("[motec] the gateway is refusing every command with 0x{:02X}; clearing it "
                    "through the management tag",
                    kStatusLatched);

        auto unlock = transport.send(encode_frame(make_unlock(++reqid)));
        if (!unlock.has_value())
        {
            return std::unexpected(unlock.error());
        }
        auto cleared = await_any_reply(transport, Code::Open, reqid, options.handshakeTimeoutMs);
        if (!cleared.has_value())
        {
            return std::unexpected(cleared.error());
        }
        if (cleared->status().value_or(0xFF) != 0)
        {
            return protocol_error(fmt::format(
                "the gateway is latched and the management tag refused to clear it (status "
                "0x{:02X}). Unplug the dongle and plug it back in -- nothing else resets it; "
                "see docs/motec_utc.md",
                cleared->status().value_or(0xFF)));
        }

        // And in again by the front door.
        ++reqid;
        auto retry = transport.send(encode_frame(make_open(reqid)));
        if (!retry.has_value())
        {
            return std::unexpected(retry.error());
        }
        opened = await_any_reply(transport, Code::Open, reqid, options.handshakeTimeoutMs);
        if (!opened.has_value())
        {
            return std::unexpected(opened.error());
        }
        SPDLOG_INFO("[motec] the gateway was latched and has been cleared");
    }

    if (opened->status().value_or(0xFF) != 0)
    {
        return protocol_error(fmt::format("the gateway refused Open with status 0x{:02X}",
                                          opened->status().value_or(0xFF)));
    }
    const uint8_t busHandle = opened->field5;
    SPDLOG_DEBUG("[motec] session open, bus handle 0x{:02X}", busHandle);

    // Version is not needed to carry traffic, but it is the cheapest proof
    // that the device is what it claims and that the envelope is being parsed
    // correctly -- a real UTC answers 7.2.
    ++reqid;
    sent = transport.send(encode_frame(make_version(busHandle, reqid)));
    if (!sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto version = await_reply(transport, Code::Version, reqid, options.handshakeTimeoutMs);
    if (!version.has_value())
    {
        return std::unexpected(version.error());
    }
    if (version->payload.size() >= 3)
    {
        SPDLOG_INFO("[motec] gateway reports version {}.{}", version->payload[1],
                    version->payload[2]);
    }

    // An accept-everything filter. Without one the device has been observed to
    // deliver nothing, and "no frames at all" is indistinguishable from a
    // quiet bus.
    ++reqid;
    sent = transport.send(encode_frame(make_accept_all_filter(busHandle, reqid)));
    if (!sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto filtered = await_reply(transport, Code::Filter, reqid, options.handshakeTimeoutMs);
    if (!filtered.has_value())
    {
        return std::unexpected(filtered.error());
    }

    return busHandle;
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

// `udp=host` or `udp=[host]:port`. Returns nullopt when the device names a
// dongle rather than a network gateway.
struct UdpTarget
{
    std::string host;
    uint16_t port { 0 };
};

std::optional<UdpTarget> parse_udp_device(const std::string& device)
{
    constexpr std::string_view kPrefix = "udp=";
    if (!device.starts_with(kPrefix))
    {
        return std::nullopt;
    }

    UdpTarget target;
    std::string rest = device.substr(kPrefix.size());

    if (rest.starts_with('['))
    {
        // Bracketed, so a v6 address's own colons cannot be mistaken for the
        // port separator.
        const size_t close = rest.find(']');
        if (close == std::string::npos)
        {
            return target; // host stays empty; open() reports it
        }
        target.host = rest.substr(1, close - 1);
        if (close + 2 < rest.size() && rest[close + 1] == ':')
        {
            unsigned int port = 0;
            const std::string text = rest.substr(close + 2);
            std::from_chars(text.data(), text.data() + text.size(), port);
            target.port = static_cast<uint16_t>(port);
        }
        return target;
    }

    // Unbracketed: a single colon is a port, and several mean a bare v6
    // address with no port at all.
    const size_t colon = rest.find(':');
    if (colon != std::string::npos && rest.find(':', colon + 1) == std::string::npos)
    {
        unsigned int port = 0;
        const std::string text = rest.substr(colon + 1);
        std::from_chars(text.data(), text.data() + text.size(), port);
        target.port = static_cast<uint16_t>(port);
        target.host = rest.substr(0, colon);
        return target;
    }

    target.host = std::move(rest);
    return target;
}

class MotecBackend : public Backend
{
public:
    explicit MotecBackend(const MotecOptions& options)
        : options_(options)
    {
    }

    const std::string& name() const override { return name_; }

    std::vector<ChannelInfo> enumerate() override
    {
        std::vector<ChannelInfo> found;
        for (const auto& device : list_usb_devices())
        {
            ChannelInfo info;
            info.id = ChannelId { "motec", std::to_string(device.index), 0 };
            // No serial in the description: ChannelInfo carries it in its own
            // field, and --list prints that, so including it here said it
            // twice.
            info.description = "MoTeC UTC";
            info.serial = device.serial;
            // Classic CAN only. Saying so here is what stops a config asking
            // for a data bit rate and finding out at open.
            info.supportsFd = false;
            info.available = device.available;
            info.unavailableReason = device.unavailableReason;
            found.push_back(std::move(info));
        }
        return found;
    }

    Result<std::shared_ptr<Channel>> open(const ChannelId& id, const OpenOptions& options) override
    {
        if (id.channel != 0)
        {
            return invalid_argument(fmt::format(
                "'{}' has a channel suffix; a MoTeC UTC is one CAN channel, so use 'motec:{}'",
                id.toString(), id.device));
        }
        if (options.bitrate.fd())
        {
            return unsupported(fmt::format(
                "'{}' asks for CAN FD; a MoTeC UTC is a classic CAN device. Remove the data bit "
                "rate from its configuration",
                id.toString()));
        }
        if (options.listenOnly)
        {
            return unsupported(fmt::format(
                "'{}' asks for listen-only, which this protocol has no command for. Nothing here "
                "can attach to a bus without acknowledging on it -- use a socketcan or pcan "
                "channel if that matters",
                id.toString()));
        }

        auto transport = open_transport(id);
        if (!transport.has_value())
        {
            return std::unexpected(transport.error());
        }

        auto busHandle = MotecChannel::handshake(**transport, options_);
        if (!busHandle.has_value())
        {
            return std::unexpected(busHandle.error());
        }

        // Said once, loudly, at the only moment anyone is looking: the number
        // in the config did not reach the hardware and cannot.
        SPDLOG_WARN("[motec] {} runs at whatever bit rate MoTeC's tool last configured; the "
                    "requested {} is NOT applied and cannot be read back",
                    id.toString(), options.bitrate.toString());

        auto channel = std::make_shared<MotecChannel>(id, *transport, options, options_, *busHandle);
        if (options.start)
        {
            auto started = channel->start();
            if (!started.has_value())
            {
                return std::unexpected(started.error());
            }
        }
        return channel;
    }

private:
    Result<std::shared_ptr<Transport>> open_transport(const ChannelId& id)
    {
        if (auto udp = parse_udp_device(id.device))
        {
            if (udp->host.empty())
            {
                return invalid_argument(fmt::format(
                    "'{}' does not name a host. Use 'motec:udp=<host>' or "
                    "'motec:udp=[<v6 address>]:<port>'",
                    id.toString()));
            }
            return open_udp_transport(udp->host, udp->port);
        }

        UsbTarget target;

        // `serial=` forces the serial reading. Without it a digit string is
        // tried as an index first and as a serial second -- see UsbTarget,
        // and note that a UTC's serial IS a bare number, so the fallback is
        // not a convenience.
        constexpr std::string_view kSerialPrefix = "serial=";
        if (id.device.starts_with(kSerialPrefix))
        {
            target.serial = id.device.substr(kSerialPrefix.size());
            if (target.serial.empty())
            {
                return invalid_argument(fmt::format(
                    "'{}' names no serial. Use 'motec:serial=<number>'", id.toString()));
            }
            return open_usb_transport(target, options_.readTimeoutMs, options_.writeTimeoutMs,
                                      options_.detachKernelDriver);
        }

        unsigned int index = 0;
        const char* begin = id.device.data();
        const char* end = begin + id.device.size();
        auto [ptr, ec] = std::from_chars(begin, end, index);
        if (ec == std::errc {} && ptr == end)
        {
            target.byIndex = true;
            target.index = index;
        }
        // Set either way: a digit string that matches no index falls back to
        // being a serial.
        target.serial = id.device;

        return open_usb_transport(target, options_.readTimeoutMs, options_.writeTimeoutMs,
                                  options_.detachKernelDriver);
    }

    MotecOptions options_;
    std::string name_ { "motec" };
};

} // namespace

std::shared_ptr<Backend> make_motec_backend(const MotecOptions& options)
{
    return std::make_shared<MotecBackend>(options);
}

} // namespace can::motec
