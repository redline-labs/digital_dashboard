// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/bus.h"

namespace canopen
{

Bus::~Bus() = default;

void Bus::subscribe(FrameHandler handler)
{
    handlers_.push_back(std::move(handler));
}

void Bus::deliver(const helpers::CanFrame& frame)
{
    // Indexed rather than range-based: a handler is allowed to subscribe
    // another one, and reallocating the vector mid-iteration would invalidate
    // an iterator. A handler added during delivery does not see the current
    // frame, which is the less surprising of the two options.
    const size_t count = handlers_.size();
    for (size_t i = 0; i < count; ++i)
    {
        if (handlers_[i])
        {
            handlers_[i](frame);
        }
    }
}

bool wait_until(Bus& bus, Duration timeout, const std::function<bool()>& predicate)
{
    if (predicate())
    {
        return true;
    }

    const Clock::time_point deadline = bus.now() + timeout;
    while (bus.now() < deadline)
    {
        // A slice rather than the whole remaining budget: a transport that
        // blocks for the full budget would otherwise overshoot a predicate
        // that came true early.
        const auto remaining
            = std::chrono::duration_cast<Duration>(deadline - bus.now());
        bus.poll(std::min(remaining, Duration { 5 }));

        if (predicate())
        {
            return true;
        }
    }

    return predicate();
}

} // namespace canopen
