#pragma once

// Turns an MTi's per-sample header into a continuous sample index and a
// device time, and says when samples went missing.
//
// Both counters on the wire wrap: the packet counter is a UInt16 (every
// 11 minutes at 100 Hz) and SampleTimeFine a UInt32 at 10 kHz (every
// 5 days). Neither wrap is an event. A counter that steps by more than one
// is -- the samples in between are gone, and the rotation that happened
// during them has to be bridged, not skipped, or the attitude jumps by
// exactly the missing rotation with nothing reporting it.

#include <cstdint>

namespace imu_preint
{

class SampleSequencer
{
  public:
    enum class Kind
    {
        first,         // no history; this sample starts the sequence
        next,          // exactly one after the last
        gap,           // `missing` samples lost before this one
        duplicate,     // same counter as the last; drop it
        out_of_order,  // behind the last; drop it
        restart,       // a jump too large to bridge; start again from here
    };

    struct Result
    {
        Kind kind = Kind::first;
        std::uint64_t index = 0;    // unwrapped packet counter
        std::uint64_t missing = 0;  // for Kind::gap
        double device_time_s = 0.0; // unwrapped SampleTimeFine
    };

    // max_bridge: the longest gap, in samples, that is worth bridging. Past
    // it the attitude change over the gap is too uncertain to integrate and
    // the caller should re-seed instead.
    explicit SampleSequencer(std::uint64_t max_bridge = 10, double tick_hz = 10000.0);

    Result push(std::uint16_t counter, std::uint32_t sample_time_fine);

  private:
    std::uint64_t max_bridge_;
    double tick_hz_;
    bool started_ = false;
    std::uint16_t last_counter_ = 0;
    std::uint32_t last_tick_ = 0;
    std::uint64_t index_ = 0;
    std::uint64_t ticks_ = 0;
};

}  // namespace imu_preint
