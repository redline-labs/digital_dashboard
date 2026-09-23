// SPDX-License-Identifier: GPL-3.0-or-later

#include "imu_assembler.h"

#include <algorithm>

namespace state_estimator
{

std::uint64_t ImuAssembler::unwrap(std::uint16_t counter)
{
    if (!last_raw_)
    {
        last_raw_ = counter;
        unwrapped_ = 1u << 20;  // headroom below, for a half that arrives late
        return unwrapped_;
    }
    // The signed modular step: halves of one sample can arrive in either
    // order, so a counter a little behind the newest is normal.
    const auto step = static_cast<std::int16_t>(static_cast<std::uint16_t>(counter - *last_raw_));
    const std::uint64_t at = static_cast<std::uint64_t>(static_cast<std::int64_t>(unwrapped_) + step);
    if (step > 0)
    {
        last_raw_ = counter;
        unwrapped_ = at;
    }
    return at;
}

void ImuAssembler::addDeltaQ(const ImuHeader& h, const Eigen::Quaterniond& dq, double arrival)
{
    const std::uint64_t k = unwrap(h.packetCounter);
    Half& half = pending_[k];
    half.counter = h.packetCounter;
    half.tick = h.sampleTimeFine;
    half.dq = dq;
    half.arrival = std::max(half.arrival, arrival);
    settle(h.packetCounter);
}

void ImuAssembler::addDeltaV(const ImuHeader& h, const Eigen::Vector3d& dv, double arrival)
{
    const std::uint64_t k = unwrap(h.packetCounter);
    Half& half = pending_[k];
    half.counter = h.packetCounter;
    half.tick = h.sampleTimeFine;
    half.dv = dv;
    half.arrival = std::max(half.arrival, arrival);
    settle(h.packetCounter);
}

void ImuAssembler::settle(std::uint16_t)
{
    // Emit complete samples in counter order; drop incomplete ones that the
    // stream has left behind.
    while (!pending_.empty())
    {
        auto it = pending_.begin();
        const bool complete = it->second.dq && it->second.dv;
        const bool abandoned = unwrapped_ - it->first > patience_;
        if (!complete && !abandoned) break;
        if (complete)
        {
            vehicle_estimator::ImuSample s;
            // The counter as the device sent it: the estimator's sequencer
            // does its own unwrapping and gap accounting.
            s.packet_counter = it->second.counter;
            s.sample_time_fine = it->second.tick;
            s.dq = *it->second.dq;
            s.dv = *it->second.dv;
            s.host_time = it->second.arrival;
            ready_.push_back(s);
        }
        else
        {
            ++orphans_;
        }
        pending_.erase(it);
    }
}

std::vector<vehicle_estimator::ImuSample> ImuAssembler::take()
{
    std::vector<vehicle_estimator::ImuSample> out;
    out.swap(ready_);
    return out;
}

}  // namespace state_estimator
