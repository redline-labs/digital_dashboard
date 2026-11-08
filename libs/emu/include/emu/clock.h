// The time base a board chip is handed: the machine's time in ticks and the tick rate, read-only.
//
// A chip that needs time (a write cycle, the watchdog window, the coil integration) holds a Clock given at wiring
// instead of a pointer to the machine, so wire/ and board/ need nothing from cpu/. A default-constructed clock is
// unconnected: now() and hz() are 0, as a chip built by hand without a machine always behaved.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "emu/common.h"

namespace sim::sys {

class Clock {
   public:
    Clock() = default;
    // `hz` points at the rate (the machine config), which may still change after wiring
    Clock(std::function<uint64_t()> now, const uint64_t* hz) : now_(std::move(now)), hz_(hz) {
    }

    bool connected() const {
        return static_cast<bool>(now_);
    }

    uint64_t now() const {
        return now_ ? now_() : 0;
    }

    uint64_t hz() const {
        return hz_ ? *hz_ : 0;
    }

    double seconds() const {
        return double(now()) / double(hz());
    }

    uint64_t ticks_per_us() const {
        return hz() / 1'000'000;
    }

   private:
    std::function<uint64_t()> now_;
    const uint64_t* hz_ = nullptr;
};

}  // namespace sim::sys
