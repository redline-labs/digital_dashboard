#include "scope/sample_ring.h"

#include <algorithm>

namespace scope
{

// ------------------------------------------------------------------ StagingRing

StagingRing::StagingRing(std::size_t capacity) :
    // One spare slot, so head == tail is unambiguously "empty". Without it,
    // a full ring and an empty one are indistinguishable.
    slots_(std::max<std::size_t>(capacity, 1) + 1),
    capacity_(std::max<std::size_t>(capacity, 1))
{
}

bool StagingRing::push(const Sample& sample)
{
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) % slots_.size();

    // Acquire: everything the consumer did before publishing this tail is
    // visible, so the slot we are about to write is genuinely free.
    if (next == tail_.load(std::memory_order_acquire))
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    slots_[head] = sample;

    // Release: the slot write above is visible to any consumer that sees this
    // head. Publishing the index before the data would let the consumer read a
    // half-written sample.
    head_.store(next, std::memory_order_release);
    return true;
}

bool StagingRing::full() const
{
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) % slots_.size();
    return next == tail_.load(std::memory_order_acquire);
}

std::size_t StagingRing::drain(std::vector<Sample>& out)
{
    const std::size_t head = head_.load(std::memory_order_acquire);
    std::size_t tail = tail_.load(std::memory_order_relaxed);

    std::size_t count = 0;
    while (tail != head)
    {
        out.push_back(slots_[tail]);
        tail = (tail + 1) % slots_.size();
        ++count;
    }

    // Release: the reads above are done before the producer may reuse the
    // slots this frees.
    tail_.store(tail, std::memory_order_release);
    return count;
}

// ---------------------------------------------------------------- SampleHistory

SampleHistory::SampleHistory(std::size_t capacity) : slots_(std::max<std::size_t>(capacity, 1))
{
}

void SampleHistory::append(const Sample& sample)
{
    if (size_ < slots_.size())
    {
        slots_[(head_ + size_) % slots_.size()] = sample;
        ++size_;
        return;
    }

    // Full: overwrite the oldest and move the window along.
    slots_[head_] = sample;
    head_ = (head_ + 1) % slots_.size();
}

std::size_t SampleHistory::lowerBound(double t_min) const
{
    // Hand-rolled rather than std::lower_bound over an iterator adaptor: the
    // storage wraps, so there is no contiguous range to hand the algorithm, and
    // a custom iterator would be more code than the six lines below.
    std::size_t low = 0;
    std::size_t high = size_;
    while (low < high)
    {
        const std::size_t mid = low + (high - low) / 2;
        if ((*this)[mid].t < t_min)
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

void SampleHistory::trimOlderThan(double t_min)
{
    const std::size_t drop = lowerBound(t_min);
    if (drop == 0)
    {
        return;
    }
    head_ = (head_ + drop) % slots_.size();
    size_ -= drop;
}

void SampleHistory::clear()
{
    head_ = 0;
    size_ = 0;
}

// ----------------------------------------------------------------- SignalBuffer

SignalBuffer::SignalBuffer(double history_seconds,
                           std::size_t max_points,
                           std::size_t staging_capacity) :
    history_seconds_(history_seconds),
    staging_(staging_capacity),
    history_(max_points)
{
    scratch_.reserve(staging_capacity);
}

void SignalBuffer::push(const Sample& sample)
{
    staging_.push(sample);
}

void SignalBuffer::drain(double now)
{
    scratch_.clear();
    const std::size_t moved = staging_.drain(scratch_);
    for (const Sample& sample : scratch_)
    {
        history_.append(sample);
    }
    received_ += moved;

    if (history_seconds_ > 0.0)
    {
        history_.trimOlderThan(now - history_seconds_);
    }
}

}  // namespace scope
