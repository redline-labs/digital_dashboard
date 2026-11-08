// The computer-operating-properly watchdog: a write to $FFFF (COPCTL) clears it; without one for (2^18 - 2^4)
// CGMXCLK cycles (65.5 ms at the PDM's 4 MHz crystal) it resets the part with SRSR.COP. CONFIG1.COPD disables it. Its
// clock stops in STOP. A read of $FFFF reads the reset vector's low byte (the flash under the register).
#pragma once

#include <cstdint>

#include "hc08/periph.h"

namespace sim::sys {

class Cop final : public Periph {
   public:
    Cop() : Periph("cop", Timing::Timed) {
    }

    void set_enabled(bool on);
    bool enabled() const {
        return enabled_;
    }

    uint64_t timeout_ticks() const;
    uint64_t next_event() const override;
    void sync(uint64_t now) override;
    void stop_mode(bool entering, uint64_t now) override;
    void reset() override;

    uint64_t feeds() const {
        return feeds_;
    }

    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   protected:
    uint8_t rd(uint16_t offset) override;
    void wr(uint16_t offset, uint8_t value) override;

   private:
    bool enabled_ = true;
    bool stopped_ = false;
    uint64_t deadline_ = kNever;
    uint64_t remaining_ = 0;  // while stopped
    uint64_t feeds_ = 0;
};

}  // namespace sim::sys
