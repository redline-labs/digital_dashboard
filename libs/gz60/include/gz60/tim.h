// The timer interface module (TIM08 reference manual; TIM1 at $0020 with 2 channels, TIM2 at $002B with 6, its
// channels 2..5 at $0456): a 16-bit counter clocked by bus / 2^PS that counts up to the modulo value and rolls over to
// 0 setting TOF; output-compare channels set CHxF when the counter matches. Input capture and the pin functions
// (PWM toggles) are not modelled; the PDM uses TIM1's overflow as its 1 ms tick and TIM2's for output 9's pulse.
//
// Lazy: the counter is a function of time since `base_tick_` (the last write, start or clock change).
// Offsets: 0 TSC, 1 CNTH, 2 CNTL, 3 MODH, 4 MODL, then 3 per channel (TSCx, CHxH, CHxL) from 5.
#pragma once

#include <array>
#include <cstdint>

#include "hc08/periph.h"
#include "emu/irq_line.h"

namespace sim::sys {

class Tim final : public Periph {
   public:
    Tim(std::string name, size_t channels) : Periph(std::move(name), Timing::Timed), channels_(channels) {
    }

    IrqLine overflow_irq;
    std::array<IrqLine, 6> channel_irq;

    uint16_t counter() const;
    uint64_t overflows() const {
        return overflows_;
    }

    uint64_t next_event() const override;
    void sync(uint64_t now) override;
    void bus_clock_changed(uint64_t now) override;
    void stop_mode(bool entering, uint64_t now) override;
    void reset() override;
    uint8_t peek(uint16_t offset) override;
    std::span<const DeviceRegister> registers() const override;

    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   protected:
    uint8_t rd(uint16_t offset) override;
    void wr(uint16_t offset, uint8_t value) override;

   private:
    bool running() const;
    uint64_t ticks_per_count() const;
    uint32_t period() const;  // counts per overflow: modulo + 1 (65536 with modulo 0)
    void rebase(uint64_t now);
    uint16_t count_at(uint64_t now) const;
    void update_irqs(uint64_t now);

    size_t channels_;
    uint8_t tsc_ = 0x20;  // TSTOP
    uint16_t modulo_ = 0xFFFF;
    uint16_t base_count_ = 0;
    uint64_t base_tick_ = 0;
    bool stopped_by_cpu_ = false;
    bool tof_ = false;
    bool tof_read_ = false;
    uint8_t latched_low_ = 0;
    bool low_latched_ = false;
    uint64_t overflows_ = 0;
    uint64_t next_overflow_ = kNever;
    std::array<uint8_t, 6> csc_{};
    std::array<uint16_t, 6> ch_{};
    std::array<bool, 6> chf_read_{};
};

}  // namespace sim::sys
