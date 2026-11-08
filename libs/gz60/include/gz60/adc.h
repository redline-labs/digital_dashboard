// The analog-to-digital converter (ADSCR, ADRH, ADRL, ADCLK at $003C..$003F; 24 channels). A write to ADSCR with a
// channel other than $1F starts a conversion: the input is sampled at that moment (the board answers `sample` with
// the pin's voltage; the PDM switches its external multiplexers on PTB just before), and the result is ready 17 ADC
// clocks later (COCO; the interrupt with AIEN). The ADC clock is bus or CGMXCLK (ADICLK) / 2^ADIV. MODE selects 8-bit
// truncated (ADRL), 10-bit right-justified, left-justified or left-justified signed. VREFH = 5 V.
//
// Reading ADRL (ADRH in the left-justified modes) or writing ADSCR clears COCO. ADCO converts continuously.
#pragma once

#include <cstdint>
#include <functional>

#include "hc08/periph.h"
#include "emu/irq_line.h"

namespace sim::sys {

class Adc final : public Periph {
   public:
    Adc() : Periph("adc", Timing::Timed) {
    }

    static constexpr double kVref = 5.0;
    static constexpr int kClocksPerConversion = 17;

    IrqLine irq;

    // The voltage at the channel's pin at `now` (ticks).
    std::function<double(int channel, uint64_t now)> sample;

    uint64_t conversions() const {
        return conversions_;
    }

    uint64_t next_event() const override;
    void sync(uint64_t now) override;
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
    uint64_t conversion_ticks() const;
    void start(uint64_t now);
    void store_result(uint16_t code10);

    uint8_t adscr_ = 0x1F;
    uint8_t adclk_ = 0;
    uint8_t adrh_ = 0;
    uint8_t adrl_ = 0;
    bool coco_ = false;
    uint16_t pending_code_ = 0;
    uint64_t done_at_ = kNever;
    uint64_t paused_remaining_ = 0;
    bool paused_ = false;
    uint64_t conversions_ = 0;
};

}  // namespace sim::sys
