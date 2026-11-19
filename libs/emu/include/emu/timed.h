// A device with behaviour in time, and the order a machine brings such devices up to date.
//
// Every machine (hc08's, e200's) runs its CPU in quanta and, between them, syncs the devices whose next event has
// come. What a device is to that loop does not depend on the CPU: it has a name, a timing class, the time of its next
// event, and sync(now) to bring itself up to `now`; reset, save and load complete it for the machine's reset and
// snapshots. A register window (a CPU's memory-mapped peripheral) is a separate concern, added by each CPU's own
// device class on top of this one.
//
//   Passive  no behaviour of its own in time: brought up to date when touched (a register access) and never scheduled
//   Timed    schedules events (a timer, the board's thermal step)
//   Input    a Timed device that drives the others (the scenario's stimulus): synced first at any tick, so a change it
//            makes at t is seen by every device synced at t
//
// Time is the machine's tick (its own rate), passed in; a device that converts it holds an emu::Clock.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "emu/common.h"

namespace sim::sys {

enum class Timing : uint8_t { Passive, Timed, Input };

class Timed {
   public:
    explicit Timed(std::string name, Timing timing = Timing::Passive) : name_(std::move(name)), timing_(timing) {
    }
    virtual ~Timed() = default;
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;

    const std::string& name() const {
        return name_;
    }
    Timing timing() const {
        return timing_;
    }

    // The tick of the next thing this device does by itself, kNever if none.
    virtual uint64_t next_event() const {
        return kNever;
    }
    // Bring the device up to `now` (process every event due at or before it).
    virtual void sync(uint64_t now) {
        (void)now;
    }
    // The machine's reset: back to the power-on state (configuration given at construction stays).
    virtual void reset() {
    }
    // Snapshots: the device's state appended to / read back from a byte stream, in a fixed order.
    virtual void save(std::vector<uint8_t>& out) const {
        (void)out;
    }
    virtual void load(const uint8_t*& in) {
        (void)in;
    }

   private:
    std::string name_;
    Timing timing_;
};

// The devices a machine schedules, Input devices first, then Timed (Passive ones are never due).
class Schedule {
   public:
    void add(Timed& device) {
        switch (device.timing()) {
            case Timing::Passive:
                return;
            case Timing::Input:
                order_.insert(order_.begin() + std::ptrdiff_t(inputs_), &device);
                inputs_++;
                return;
            case Timing::Timed:
                order_.push_back(&device);
                return;
        }
    }

    // The earliest next event of any scheduled device.
    uint64_t earliest() const {
        uint64_t earliest = kNever;
        for (const Timed* device : order_) {
            earliest = std::min(earliest, device->next_event());
        }
        return earliest;
    }

    // Syncs every device due at `t`, in repeated passes: a device synced may make another due (the stimulus sets an
    // input the ADC samples).
    void sync_due(uint64_t t) {
        for (int pass = 0; pass < kMaxPasses; pass++) {
            bool any = false;
            for (Timed* device : order_) {
                if (device->next_event() <= t) {
                    device->sync(t);
                    any = true;
                }
            }
            if (!any) {
                return;
            }
        }
        SPDLOG_WARN("devices still due after {} passes at t={}", kMaxPasses, t);
    }

    const std::vector<Timed*>& order() const {
        return order_;
    }

   private:
    static constexpr int kMaxPasses = 64;
    std::vector<Timed*> order_;
    size_t inputs_ = 0;
};

}  // namespace sim::sys
