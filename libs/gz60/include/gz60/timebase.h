// The timebase module (TBCR at $001C): a periodic interrupt from CGMXCLK divided by 2^15, 2^13, 2^11, 2^7, 2^6, 2^4,
// 2^3 or 2^2 (TBR2:0). It keeps running in STOP (the PDM's sleep wakes on it to evaluate the logic), so it is one of
// the STOP wake-up sources. TBIF sets each period; writing TACK clears it.
#pragma once

#include <cstdint>

#include "hc08/periph.h"
#include "emu/irq_line.h"

namespace sim::sys {

class Timebase final : public Periph {
   public:
    Timebase() : Periph("timebase", Timing::Timed) {
    }

    IrqLine irq;

    uint64_t period_ticks() const;
    uint64_t next_event() const override;
    void sync(uint64_t now) override;
    void reset() override;
    uint8_t peek(uint16_t offset) override;

    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   protected:
    uint8_t rd(uint16_t offset) override;
    void wr(uint16_t offset, uint8_t value) override;

   private:
    uint8_t tbcr_ = 0;  // TBR, TBIE, TBON (TBIF kept apart)
    bool tbif_ = false;
    uint64_t next_ = kNever;
};

}  // namespace sim::sys
