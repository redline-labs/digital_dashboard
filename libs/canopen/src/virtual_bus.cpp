// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/virtual_bus.h"

#include <algorithm>

namespace canopen
{

VirtualBus::VirtualBus(uint32_t bitrateKbps)
    : bitrateKbps_(bitrateKbps)
{
}

void VirtualBus::send(const helpers::CanFrame& frame)
{
    sent_.push_back(frame);

    // Indexed, because a device is allowed to attach another one -- a keypad
    // model that grows an LSS responder when it enters configuration mode, for
    // instance -- and reallocation would invalidate an iterator.
    const size_t count = devices_.size();
    for (size_t i = 0; i < count; ++i)
    {
        if (devices_[i])
        {
            devices_[i](frame);
        }
    }
}

void VirtualBus::poll(Duration budget)
{
    now_ += budget;

    // Deliver in scheduled order, and take a copy of the due set first: a
    // handler may inject more frames, and those belong to a later poll rather
    // than to this one.
    std::stable_sort(queue_.begin(), queue_.end(),
                     [](const Scheduled& a, const Scheduled& b) { return a.at < b.at; });

    std::vector<helpers::CanFrame> due;
    auto split = std::find_if(queue_.begin(), queue_.end(),
                              [this](const Scheduled& s) { return s.at > now_; });
    for (auto it = queue_.begin(); it != split; ++it)
    {
        // A frame sent at one bit rate is not received at another. Dropping it
        // here is how a client that reconfigured a device's bit rate and did
        // not follow it finds out.
        if (it->senderBitrateKbps != 0 && it->senderBitrateKbps != bitrateKbps_)
        {
            continue;
        }
        due.push_back(it->frame);
    }
    queue_.erase(queue_.begin(), split);

    for (const auto& frame : due)
    {
        deliver(frame);
    }
}

Clock::time_point VirtualBus::now() const
{
    return now_;
}

void VirtualBus::attach(Device device)
{
    devices_.push_back(std::move(device));
}

void VirtualBus::inject(const helpers::CanFrame& frame, Duration after,
                        uint32_t senderBitrateKbps)
{
    queue_.push_back(Scheduled { now_ + after, frame, senderBitrateKbps });
}

} // namespace canopen
