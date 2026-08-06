#include "scope/capture_buffer.h"

#include <algorithm>

namespace scope
{

namespace
{

constexpr double kNanosPerSecond = 1e9;

}  // namespace

CaptureBuffer::CaptureBuffer(std::size_t max_bytes, double max_seconds) :
    max_bytes_(max_bytes), max_seconds_(max_seconds)
{
}

std::size_t CaptureBuffer::sizeOf(const bag::QueuedMessage& message)
{
    return message.payload.size() + message.key.size() + message.schema.size() +
           message.origin_zid.size();
}

void CaptureBuffer::push(bag::QueuedMessage message)
{
    const std::lock_guard<std::mutex> guard(mutex_);

    bytes_ += sizeOf(message);
    messages_.push_back(std::move(message));
    ++revision_;

    evictLocked();
}

void CaptureBuffer::setBounds(std::size_t max_bytes, double max_seconds)
{
    const std::lock_guard<std::mutex> guard(mutex_);
    max_bytes_ = max_bytes;
    max_seconds_ = max_seconds;
    evictLocked();
}

void CaptureBuffer::evictLocked()
{
    // BYTES FIRST, then time, and both every push -- not one or the other. A
    // single 4 MB video frame can put the buffer over the byte cap on its own
    // while the retained span is still well inside the time cap, and a capture
    // that only checked whichever bound it expected to hit would be OOM-killed
    // by the other.
    while (max_bytes_ > 0 && bytes_ > max_bytes_ && !messages_.empty())
    {
        const std::size_t evicted = sizeOf(messages_.front());
        bytes_ -= evicted;
        evicted_bytes_ += evicted;
        ++evicted_;
        messages_.pop_front();
        ++revision_;
    }

    if (max_seconds_ > 0.0 && !messages_.empty())
    {
        const std::uint64_t newest = messages_.back().log_time_ns;
        const auto span_ns = static_cast<std::uint64_t>(max_seconds_ * kNanosPerSecond);

        // Guarded rather than assumed: log_time is taken from the recorder's own
        // monotone clock, but a message replayed onto the bus or one arriving
        // from a test can be older than `span_ns` from the epoch, and the
        // unsigned subtraction would wrap into a cutoff near UINT64_MAX --
        // silently evicting the entire buffer on every push.
        if (newest > span_ns)
        {
            const std::uint64_t cutoff = newest - span_ns;
            while (!messages_.empty() && messages_.front().log_time_ns < cutoff)
            {
                const std::size_t evicted = sizeOf(messages_.front());
                bytes_ -= evicted;
                evicted_bytes_ += evicted;
                ++evicted_;
                messages_.pop_front();
                ++revision_;
            }
        }
    }
}

void CaptureBuffer::forEach(std::uint64_t t0_ns, std::uint64_t t1_ns,
                            const std::function<void(const bag::QueuedMessage&)>& visit) const
{
    const std::lock_guard<std::mutex> guard(mutex_);

    for (const bag::QueuedMessage& message : messages_)
    {
        if (message.log_time_ns < t0_ns)
        {
            continue;
        }
        if (message.log_time_ns > t1_ns)
        {
            // Messages are pushed in arrival order, so nothing later can be in
            // range either.
            break;
        }
        visit(message);
    }
}

std::pair<std::uint64_t, std::uint64_t> CaptureBuffer::spanNanos() const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    if (messages_.empty())
    {
        return {0, 0};
    }
    return {messages_.front().log_time_ns, messages_.back().log_time_ns};
}

std::uint64_t CaptureBuffer::revision() const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    return revision_;
}

std::size_t CaptureBuffer::size() const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    return messages_.size();
}

std::size_t CaptureBuffer::bytes() const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    return bytes_;
}

std::uint64_t CaptureBuffer::evicted() const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    return evicted_;
}

std::uint64_t CaptureBuffer::evictedBytes() const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    return evicted_bytes_;
}

double CaptureBuffer::retainedSpanSeconds() const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    if (messages_.size() < 2)
    {
        return 0.0;
    }
    return static_cast<double>(messages_.back().log_time_ns - messages_.front().log_time_ns) /
           kNanosPerSecond;
}

void CaptureBuffer::clear()
{
    const std::lock_guard<std::mutex> guard(mutex_);

    // The eviction counters are NOT reset. They are lifetime totals, and a
    // capture that zeroed them on clear would let "this session lost nothing"
    // and "this session was just restarted" look identical.
    messages_.clear();
    bytes_ = 0;
    ++revision_;
}

}  // namespace scope
