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
                            const std::function<bool(const bag::QueuedMessage&)>& visit) const
{
    // The lock is DROPPED and retaken every kChunk messages. A decode pass over
    // a 1 GiB capture visits millions of entries, and one lock_guard around the
    // whole walk starves everything else that needs this mutex for the duration
    // -- the RX thread's push() while capturing, and the transport bar's stat
    // reads at the render rate while reviewing. Between chunks the cursor is
    // re-found by binary search on log_time (arrival order is non-decreasing),
    // and a chunk only breaks at a TIMESTAMP BOUNDARY so the resume point
    // `last_t + 1` can neither skip nor revisit a message. Messages pushed or
    // evicted mid-pass are seen or missed exactly as a bag growing underneath
    // a reader would be.
    constexpr std::size_t kChunk = 1024;

    std::uint64_t resume_from = t0_ns;
    while (true)
    {
        const std::lock_guard<std::mutex> guard(mutex_);

        auto it = std::lower_bound(messages_.begin(), messages_.end(), resume_from,
                                   [](const bag::QueuedMessage& message, std::uint64_t t)
                                   { return message.log_time_ns < t; });

        std::size_t visited = 0;
        std::uint64_t last_t = 0;
        for (; it != messages_.end(); ++it)
        {
            if (it->log_time_ns > t1_ns)
            {
                // Arrival order, so nothing later can be in range either.
                return;
            }
            if (visited >= kChunk && it->log_time_ns != last_t)
            {
                break;
            }
            if (!visit(*it))
            {
                return;
            }
            last_t = it->log_time_ns;
            ++visited;
        }

        if (it == messages_.end())
        {
            return;
        }
        resume_from = last_t + 1;
    }
}

void CaptureBuffer::density(std::uint64_t t0_ns, std::uint64_t t1_ns, std::size_t buckets,
                            std::vector<std::uint32_t>& out) const
{
    out.assign(buckets, 0);
    if (buckets == 0 || t1_ns <= t0_ns)
    {
        return;
    }

    const std::uint64_t span = t1_ns - t0_ns;

    const std::lock_guard<std::mutex> guard(mutex_);

    // Binary search to the range's start rather than skipping message by
    // message: the deque is in arrival order and random-access, so the walk is
    // O(range + log retained). Counting a 300 s window out of a 1800 s
    // retention linearly would spend five sixths of the lock hold skipping.
    auto it = std::lower_bound(messages_.begin(), messages_.end(), t0_ns,
                               [](const bag::QueuedMessage& message, std::uint64_t t)
                               { return message.log_time_ns < t; });
    for (; it != messages_.end(); ++it)
    {
        const bag::QueuedMessage& message = *it;
        if (message.log_time_ns > t1_ns)
        {
            // Arrival order, so nothing later can be in range either.
            break;
        }

        // In nanoseconds throughout, and the multiply comes FIRST. Computing a
        // fraction in doubles first loses resolution at the far end of a long
        // recording -- a double holds about 15 significant digits and a UNIX
        // nanosecond timestamp already uses 19 -- so buckets near the end would
        // quietly collect the wrong messages. The product overflows only past
        // roughly 200 days of span at a thousand buckets, which no capture
        // bounded by max_capture_seconds can reach.
        const std::uint64_t offset = message.log_time_ns - t0_ns;
        std::size_t bucket = static_cast<std::size_t>((offset * buckets) / span);

        // The last bucket is closed at the top, so a message landing exactly on
        // t1 belongs to it rather than to a bucket that does not exist.
        if (bucket >= buckets)
        {
            bucket = buckets - 1;
        }
        ++out[bucket];
    }
}

CaptureBuffer::Stats CaptureBuffer::stats() const
{
    const std::lock_guard<std::mutex> guard(mutex_);

    Stats out;
    out.messages = messages_.size();
    out.bytes = bytes_;
    out.evicted = evicted_;
    out.revision = revision_;
    if (messages_.size() >= 2)
    {
        out.retained_span_seconds =
            static_cast<double>(messages_.back().log_time_ns - messages_.front().log_time_ns) /
            kNanosPerSecond;
    }
    return out;
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
