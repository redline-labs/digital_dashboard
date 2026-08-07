// SPDX-License-Identifier: GPL-3.0-or-later

#include "trc_recorder.h"

#include "helpers/rate_gate.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

namespace can_bridge
{

namespace
{

using Clock = std::chrono::steady_clock;

uint64_t wall_clock_us()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// Turns a frame into the record kind the format has for it. An error frame from
// a controller is not traffic, and writing it as a data frame would put a
// message on the trace from a device that does not exist.
can::trc::RecordKind kind_for(const helpers::CanFrame& frame)
{
    if (frame.isError)
    {
        return can::trc::RecordKind::ErrorFrame;
    }
    if (frame.isRTR)
    {
        return can::trc::RecordKind::Remote;
    }
    return can::trc::RecordKind::Data;
}

} // namespace

can::Result<std::unique_ptr<TrcRecorder>> TrcRecorder::create(const std::string& path, uint8_t bus,
                                                              const can::trc::BusInfo& busInfo)
{
    can::trc::WriterOptions options;
    options.startTimeUnixUs = wall_clock_us();
    options.generatedBy = "Redline can_bridge";
    options.buses.push_back(busInfo);

    auto writer = can::trc::Writer::create(path, options);
    if (!writer.has_value())
    {
        return std::unexpected(writer.error());
    }

    auto recorder = std::unique_ptr<TrcRecorder>(new TrcRecorder());
    recorder->writer_ = std::move(*writer);
    recorder->path_ = path;
    recorder->bus_ = bus;
    recorder->startSteady_ = Clock::now();
    recorder->writerThread_ = std::thread([raw = recorder.get()] { raw->run(); });
    return recorder;
}

TrcRecorder::~TrcRecorder()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wakeup_.notify_all();
    if (writerThread_.joinable())
    {
        writerThread_.join();
    }
    if (writer_)
    {
        (void)writer_->flush();
    }
}

void TrcRecorder::record_rx(const helpers::CanFrame& frame)
{
    enqueue(frame, false);
}

void TrcRecorder::record_tx(const helpers::CanFrame& frame)
{
    enqueue(frame, true);
}

uint64_t TrcRecorder::offset_for(const helpers::CanFrame& frame, bool isTx)
{
    const uint64_t arrivalUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - startSteady_).count());

    // A transmitted frame is built from a zenoh message and has no hardware
    // stamp to offer, so it is placed where it was handed over. That is a
    // different clock from a received frame's, and the two are reconciled by
    // both being expressed against this trace's own origin.
    if (isTx || frame.timestampUs == 0)
    {
        return arrivalUs;
    }

    if (!haveHardwareOrigin_)
    {
        haveHardwareOrigin_ = true;
        hardwareOriginUs_ = frame.timestampUs;
        hardwareOriginOffsetUs_ = arrivalUs;
        return arrivalUs;
    }

    // Backwards means the adapter's counter wrapped, or two backends' stamps
    // reached one recorder. Neither is something to guess about: fall back to
    // arrival time, which is always monotonic, rather than emit an offset that
    // makes the trace non-monotonic and every reader of it wrong.
    if (frame.timestampUs < hardwareOriginUs_)
    {
        return arrivalUs;
    }

    return hardwareOriginOffsetUs_ + (frame.timestampUs - hardwareOriginUs_);
}

void TrcRecorder::enqueue(const helpers::CanFrame& frame, bool isTx)
{
    can::trc::Record record;
    record.kind = kind_for(frame);
    record.bus = bus_;
    record.isTx = isTx;
    record.frame = frame;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kQueueDepth)
        {
            // Drop this one rather than the oldest. A trace is read forwards
            // from a point of interest, so losing the far end of a backlog is
            // less damaging than losing the frame that led up to it -- and
            // either way the count is what says the trace has a hole. Nothing
            // is numbered here, so the numbers in the file stay contiguous and
            // recordDropped is what says frames are missing.
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Stamping and queueing happen under one lock on purpose. With two --
        // the pump thread and a zenoh callback both record here -- a producer
        // that took its offset and was preempted before pushing would land
        // behind one that took a later offset, and the file would come out
        // with its offsets going backwards. That is worse than a gap: several
        // things in this tree binary-search buffers that assume time does not
        // move backwards and cannot detect it when it does.
        record.offsetUs = offset_for(frame, isTx);
        record.number = ++number_;
        queue_.push_back(std::move(record));
    }
    wakeup_.notify_one();
}

void TrcRecorder::run()
{
    // A stalled disk on a busy bus produces one of these per frame otherwise.
    helpers::RateGate dropWarning { std::chrono::seconds(5) };
    uint64_t reportedDrops = 0;

    std::deque<can::trc::Record> batch;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeup_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_)
            {
                break;
            }
            batch.swap(queue_);
        }

        for (const can::trc::Record& record : batch)
        {
            auto written = writer_->write(record);
            if (!written.has_value())
            {
                SPDLOG_ERROR("trc recorder: writing '{}' failed: {}", path_,
                             written.error().message);
                continue;
            }
            recorded_.fetch_add(1, std::memory_order_relaxed);
        }
        batch.clear();

        const uint64_t drops = dropped_.load(std::memory_order_relaxed);
        if (drops != reportedDrops && dropWarning.ready(Clock::now()))
        {
            dropWarning.mark(Clock::now());
            SPDLOG_WARN("trc recorder: '{}' has dropped {} frames; the trace has holes in it",
                        path_, drops);
            reportedDrops = drops;
        }
    }

    (void)writer_->flush();
}

} // namespace can_bridge
