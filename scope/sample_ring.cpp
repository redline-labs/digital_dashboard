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
    std::size_t next = head + 1;
    if (next == slots_.size())
    {
        next = 0;
    }

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
    std::size_t next = head + 1;
    if (next == slots_.size())
    {
        next = 0;
    }
    return next == tail_.load(std::memory_order_acquire);
}

std::size_t StagingRing::drain(std::vector<Sample>& out)
{
    return drainEach([&out](const Sample& sample) { out.push_back(sample); });
}

// ---------------------------------------------------------------- SampleHistory

SampleHistory::SampleHistory(std::size_t capacity) : slots_(std::max<std::size_t>(capacity, 1))
{
}

void SampleHistory::append(const Sample& sample)
{
    if (size_ < slots_.size())
    {
        std::size_t at = head_ + size_;
        if (at >= slots_.size())
        {
            at -= slots_.size();
        }
        slots_[at] = sample;
        ++size_;
        return;
    }

    // Full: overwrite the oldest and move the window along.
    slots_[head_] = sample;
    if (++head_ == slots_.size())
    {
        head_ = 0;
    }
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
    head_ += drop;
    if (head_ >= slots_.size())
    {
        head_ -= slots_.size();
    }
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
}

void SignalBuffer::push(const Sample& sample)
{
    staging_.push(sample);
}

std::size_t SignalBuffer::drain(double now)
{
    // Straight from the ring into the history -- no intermediate vector, no
    // copy per sample beyond the one the hand-off requires.
    const std::size_t moved = staging_.drainEach([this](const Sample& sample)
                                                 { history_.append(sample); });
    received_ += moved;

    if (history_seconds_ > 0.0)
    {
        history_.trimOlderThan(now - history_seconds_);
    }
    return moved;
}

void SignalBuffer::clear()
{
    // The staging ring goes too. Anything left in it was produced before the
    // seek and would land in the history on the next drain, one sample at a
    // time, out of order with the window that was just loaded -- which is
    // precisely the non-decreasing-time violation lowerBound() cannot survive.
    (void)staging_.drainEach([](const Sample&) {});

    history_.clear();
}

void SignalBuffer::replaceHistory(std::span<const Sample> samples)
{
    clear();
    append(samples);
}

void SignalBuffer::append(std::span<const Sample> samples)
{
    for (const Sample& sample : samples)
    {
        history_.append(sample);
    }
    received_ += samples.size();
}

}  // namespace scope
