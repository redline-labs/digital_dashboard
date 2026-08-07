// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_trc/trc_backend.h"

#include "can_trc/trc.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

namespace can::trc
{

namespace
{

using Clock = std::chrono::steady_clock;

// Separates one pass of a looped trace from the next, so no two frames share a
// timestamp across the seam.
constexpr uint64_t kLoopGapUs = 1000;

class TrcChannel : public Channel
{
public:
    TrcChannel(ChannelId id, std::string path, ReplayOptions options, Bitrate bitrate,
               bool listenOnly)
        : id_ { std::move(id) }
        , path_ { std::move(path) }
        , options_ { options }
        , bitrate_ { bitrate }
        , listenOnly_ { listenOnly }
        , description_ { fmt::format("TRC replay of {}", path_) }
    {
    }

    const ChannelId& id() const override { return id_; }
    const std::string& description() const override { return description_; }

    // A file has no bit rate to set, but remembering the one asked for costs
    // nothing and lets a recorder downstream put it in the header it writes.
    Result<void> set_bitrate(const Bitrate& bitrate) override
    {
        bitrate_ = bitrate;
        return {};
    }
    Bitrate bitrate() const override { return bitrate_; }
    bool supports_fd() const override { return true; }

    Result<void> set_listen_only(bool listenOnly) override
    {
        listenOnly_ = listenOnly;
        return {};
    }
    bool listen_only() const override { return listenOnly_; }

    Result<void> start() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto opened = open_reader();
        if (!opened.has_value())
        {
            return std::unexpected(opened.error());
        }
        stopping_ = false;
        running_ = true;
        exhausted_ = false;
        return {};
    }

    Result<void> stop() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            stopping_ = true;
        }
        // A trace can have seconds between frames. Without this a shutdown
        // waits out the gap, which is the same bug the virtual backend's
        // condvar exists to avoid.
        wakeup_.notify_all();
        return {};
    }

    bool running() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    Result<void> send(const helpers::CanFrame& frame) override
    {
        // Nothing is listening to a file. Counting it says so out loud rather
        // than pretending the frame went somewhere.
        (void)frame;
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.txDropped;
        return {};
    }

    Result<size_t> receive(std::span<helpers::CanFrame> out, Duration timeout) override
    {
        if (out.empty())
        {
            return size_t { 0 };
        }

        const Clock::time_point deadline = Clock::now() + timeout;
        std::unique_lock<std::mutex> lock(mutex_);

        size_t delivered = 0;
        while (delivered < out.size())
        {
            if (!running_ || stopping_)
            {
                break;
            }
            if (!pending_.has_value() && !fetch_next())
            {
                // End of the trace with no loop. A finished file is a quiet
                // bus, not an error -- the caller keeps polling and keeps
                // getting nothing, exactly as it would from unplugged cable.
                break;
            }

            const Clock::time_point due = due_time(*pending_);
            const Clock::time_point now = Clock::now();
            if (now < due)
            {
                if (delivered > 0)
                {
                    // Hand over what is ready rather than holding it back for
                    // the sake of a fuller batch.
                    break;
                }
                if (due >= deadline)
                {
                    wakeup_.wait_until(lock, deadline);
                    break;
                }
                wakeup_.wait_until(lock, due);
                if (stopping_)
                {
                    break;
                }
                continue;
            }

            out[delivered] = pending_->frame;
            ++delivered;
            ++stats_.rxFrames;
            stats_.rxBytes += pending_->frame.len;
            if (pending_->frame.isError)
            {
                ++stats_.errorFrames;
            }
            pending_.reset();
        }

        return delivered;
    }

    Statistics statistics() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Statistics copy = stats_;
        copy.state = running_ ? (exhausted_ ? BusState::Stopped : BusState::ErrorActive)
                              : BusState::Stopped;
        return copy;
    }

private:
    Result<void> open_reader()
    {
        auto reader = Reader::open(path_);
        if (!reader.has_value())
        {
            return std::unexpected(reader.error());
        }
        reader_ = std::move(*reader);
        haveFirstOffset_ = false;
        return {};
    }

    // Pulls records until one belongs to this channel's bus and can be carried
    // as a frame. Returns false when the trace is finished and not looping.
    bool fetch_next()
    {
        for (;;)
        {
            if (!reader_)
            {
                return false;
            }

            auto record = reader_->next();
            if (!record.has_value())
            {
                SPDLOG_WARN("[{}] {}", id_.toString(), record.error().message);
                exhausted_ = true;
                return false;
            }

            if (!record->has_value())
            {
                if (!options_.loop)
                {
                    if (!exhausted_)
                    {
                        exhausted_ = true;
                        SPDLOG_INFO("[{}] end of trace: {} records, {} bad lines",
                                    id_.toString(), reader_->stats().records,
                                    reader_->stats().badLines);
                    }
                    return false;
                }

                // Start the next pass beyond the last one, so a consumer that
                // assumes non-decreasing time -- and this tree has several --
                // does not see the clock jump backwards at the seam.
                replayEpochUs_ += (lastOffsetUs_ - firstOffsetUs_) + kLoopGapUs;
                paceOrigin_ = Clock::now();
                auto reopened = open_reader();
                if (!reopened.has_value())
                {
                    SPDLOG_ERROR("[{}] cannot reopen for looping: {}", id_.toString(),
                                 reopened.error().message);
                    exhausted_ = true;
                    return false;
                }
                continue;
            }

            Record& value = **record;

            // Bus 0 on the channel id means "whatever the file has". A trace
            // with no bus column reports bus 0 on every record, so an
            // unqualified trc:<path> reads it and trc:<path>/1 does not, which
            // is the right way round: the file never claimed to be bus 1.
            if (id_.channel != 0 && value.bus != id_.channel)
            {
                continue;
            }

            switch (value.kind)
            {
                case RecordKind::Data:
                case RecordKind::Remote:
                case RecordKind::ErrorFrame:
                case RecordKind::HardwareStatus:
                case RecordKind::ErrorCounter:
                    break;
                case RecordKind::Event:
                    // Text with no frame to put it in.
                    SPDLOG_DEBUG("[{}] event at {} us: {}", id_.toString(), value.offsetUs,
                                 value.event);
                    continue;
                case RecordKind::Unsupported:
                    continue;
            }

            if (!haveFirstOffset_)
            {
                haveFirstOffset_ = true;
                firstOffsetUs_ = value.offsetUs;
                lastOffsetUs_ = value.offsetUs;
                if (replayEpochUs_ == 0)
                {
                    // The first pass keeps the trace's own absolute time when
                    // it had one. This is the only backend in the tree that can
                    // produce a real UNIX-epoch stamp: socketcan gives the
                    // kernel's wall clock, the PCAN adapter gives its own
                    // uptime, and a virtual bus gives nothing.
                    replayEpochUs_ = reader_->header().startTimeUnixUs != 0
                        ? reader_->header().startTimeUnixUs + firstOffsetUs_
                        : wallClockAtOpenUs_;
                    paceOrigin_ = Clock::now();
                }
            }
            lastOffsetUs_ = value.offsetUs;

            value.frame.timestampUs = replayEpochUs_ + (value.offsetUs - firstOffsetUs_);
            pending_ = std::move(value);
            return true;
        }
    }

    Clock::time_point due_time(const Record& record) const
    {
        if (!options_.paced)
        {
            return Clock::time_point::min();
        }
        const double speed = options_.speed > 0.0 ? options_.speed : 1.0;
        const auto elapsedUs
            = static_cast<int64_t>(static_cast<double>(record.offsetUs - firstOffsetUs_) / speed);
        return paceOrigin_ + std::chrono::microseconds(elapsedUs);
    }

    ChannelId id_;
    std::string path_;
    ReplayOptions options_;
    Bitrate bitrate_;
    bool listenOnly_ { false };
    std::string description_;

    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    bool running_ { false };
    bool stopping_ { false };
    bool exhausted_ { false };

    std::unique_ptr<Reader> reader_;
    std::optional<Record> pending_;
    Statistics stats_;

    bool haveFirstOffset_ { false };
    uint64_t firstOffsetUs_ { 0 };
    uint64_t lastOffsetUs_ { 0 };
    uint64_t replayEpochUs_ { 0 };
    uint64_t wallClockAtOpenUs_ { static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count()) };
    Clock::time_point paceOrigin_ { Clock::now() };
};

class TrcBackend : public Backend
{
public:
    explicit TrcBackend(const ReplayOptions& options)
        : options_ { options }
    {
    }

    const std::string& name() const override { return name_; }

    // Nothing to enumerate: a trace exists because someone names a path, the
    // same reason virtual: reports no buses until one is opened.
    std::vector<ChannelInfo> enumerate() override { return {}; }

    Result<std::shared_ptr<Channel>> open(const ChannelId& id,
                                          const OpenOptions& options) override
    {
        if (id.device.empty())
        {
            return invalid_argument("trc: needs a path, as trc:/logs/run.trc");
        }

        // Opening the file here rather than at the first receive() means a
        // typo in a path is reported where every other backend reports a
        // missing device, instead of as a bus that is simply always quiet.
        auto probe = Reader::open(id.device);
        if (!probe.has_value())
        {
            return std::unexpected(probe.error());
        }

        auto channel = std::make_shared<TrcChannel>(id, id.device, options_, options.bitrate,
                                                    options.listenOnly);
        if (options.start)
        {
            auto started = channel->start();
            if (!started.has_value())
            {
                return std::unexpected(started.error());
            }
        }
        return std::static_pointer_cast<Channel>(channel);
    }

private:
    ReplayOptions options_;
    std::string name_ { "trc" };
};

} // namespace

std::shared_ptr<Backend> make_trc_backend(const ReplayOptions& options)
{
    return std::make_shared<TrcBackend>(options);
}

} // namespace can::trc
