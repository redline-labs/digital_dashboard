#include "scope/raw_buffer.h"

#include <algorithm>
#include <utility>

namespace scope
{

std::size_t rawMessageBytes(const RawMessage& message)
{
    return message.payload.size() + sizeof(RawMessage);
}

// ------------------------------------------------------------------ RawHistory

void RawHistory::append(RawMessage message)
{
    bytes_ += rawMessageBytes(message);
    messages_.push_back(std::move(message));
}

void RawHistory::replace(std::vector<RawMessage> messages)
{
    clear();
    for (RawMessage& message : messages)
    {
        append(std::move(message));
    }
}

std::size_t RawHistory::lowerBound(double t_min) const
{
    // Hand-rolled rather than std::lower_bound over the deque because the
    // comparison is on a member and the deque's iterators are not contiguous;
    // this is the same shape SampleHistory uses and stays O(log n).
    std::size_t low = 0;
    std::size_t high = messages_.size();
    while (low < high)
    {
        const std::size_t mid = low + (high - low) / 2;
        if (messages_[mid].t < t_min)
        {
            low = mid + 1;
        }
        else
        {
            high = mid;
        }
    }
    return low;
}

void RawHistory::trim(double now, double history_seconds, std::size_t max_bytes)
{
    // Time first. A zero or negative retention means "no time bound", which is
    // what a caller that only wants the byte cap passes.
    if (history_seconds > 0.0)
    {
        const double oldest_kept = now - history_seconds;
        while (!messages_.empty() && messages_.front().t < oldest_kept)
        {
            bytes_ -= rawMessageBytes(messages_.front());
            messages_.pop_front();
        }
    }

    // Then bytes, so whichever bound binds first wins. Zero disables it, matching
    // scope_workspace_t's capture caps.
    if (max_bytes > 0)
    {
        while (bytes_ > max_bytes && !messages_.empty())
        {
            bytes_ -= rawMessageBytes(messages_.front());
            messages_.pop_front();
        }
    }
}

void RawHistory::clear()
{
    messages_.clear();
    bytes_ = 0;
}

// ------------------------------------------------------------------- RawBuffer

RawBuffer::RawBuffer(double history_seconds, std::size_t max_bytes)
    : history_seconds_(history_seconds), max_bytes_(max_bytes)
{
}

void RawBuffer::push(RawMessage message)
{
    const std::size_t size = rawMessageBytes(message);

    const std::lock_guard<std::mutex> guard(mutex_);

    // Drop the newest rather than evicting the oldest: evicting here would mean
    // the producer moving a position the consumer owns. See the header.
    if (max_bytes_ > 0 && staging_bytes_ + size > max_bytes_ && !staging_.empty())
    {
        ++dropped_;
        return;
    }

    staging_bytes_ += size;
    staging_.push_back(std::move(message));
}

void RawBuffer::drain(double now)
{
    std::deque<RawMessage> staged;
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        staged.swap(staging_);
        staging_bytes_ = 0;
    }

    for (RawMessage& message : staged)
    {
        ++received_;
        history_.append(std::move(message));
    }

    history_.trim(now, history_seconds_, max_bytes_);
}

void RawBuffer::replaceHistory(std::vector<RawMessage> messages)
{
    ++generation_;
    received_ += messages.size();
    history_.replace(std::move(messages));

    // No trim here. The caller decided what window it wanted, and trimming
    // against `now` would immediately discard the far side of a backwards seek
    // -- which is precisely the data it just asked for. The byte bound is the
    // caller's to respect when it chooses the window; RecordedSource does.
}

void RawBuffer::append(std::vector<RawMessage> messages)
{
    for (RawMessage& message : messages)
    {
        ++received_;
        history_.append(std::move(message));
    }
}

void RawBuffer::clear()
{
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        staging_.clear();
        staging_bytes_ = 0;
    }
    history_.clear();

    // A clear is a replacement too, as far as a consumer decoding from this is
    // concerned: everything it had decoded is gone.
    ++generation_;
}

std::uint64_t RawBuffer::dropped() const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    return dropped_;
}

void RawBuffer::setBounds(double history_seconds, std::size_t max_bytes)
{
    history_seconds_ = history_seconds;
    max_bytes_ = max_bytes;
}

}  // namespace scope
