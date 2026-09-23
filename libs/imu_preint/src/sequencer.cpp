#include "imu_preint/sequencer.h"

namespace imu_preint
{

SampleSequencer::SampleSequencer(std::uint64_t max_bridge, double tick_hz) : max_bridge_(max_bridge), tick_hz_(tick_hz)
{
}

SampleSequencer::Result SampleSequencer::push(std::uint16_t counter, std::uint32_t sample_time_fine)
{
    Result r;
    if (!started_)
    {
        started_ = true;
        last_counter_ = counter;
        last_tick_ = sample_time_fine;
        index_ = counter;
        ticks_ = sample_time_fine;
        r.kind = Kind::first;
        r.index = index_;
        r.device_time_s = static_cast<double>(ticks_) / tick_hz_;
        return r;
    }

    // Unsigned subtraction is the modular difference: a wrap is a step of 1.
    const std::uint16_t step = static_cast<std::uint16_t>(counter - last_counter_);
    if (step == 0)
    {
        r.kind = Kind::duplicate;
        r.index = index_;
        r.device_time_s = static_cast<double>(ticks_) / tick_hz_;
        return r;
    }
    if (step >= 0x8000)
    {
        r.kind = Kind::out_of_order;
        r.index = index_;
        r.device_time_s = static_cast<double>(ticks_) / tick_hz_;
        return r;
    }

    const std::uint32_t tick_step = sample_time_fine - last_tick_;
    last_counter_ = counter;
    last_tick_ = sample_time_fine;
    index_ += step;
    ticks_ += tick_step;
    r.index = index_;
    r.device_time_s = static_cast<double>(ticks_) / tick_hz_;
    if (step == 1)
    {
        r.kind = Kind::next;
    }
    else if (step - 1u <= max_bridge_)
    {
        r.kind = Kind::gap;
        r.missing = step - 1u;
    }
    else
    {
        r.kind = Kind::restart;
        r.missing = step - 1u;
    }
    return r;
}

}  // namespace imu_preint
