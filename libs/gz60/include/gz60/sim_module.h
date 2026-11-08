// The System Integration Module's registers: reset status (SRSR), the interrupt status registers (INT1..INT3, which
// mirror the pending sources), the break module's registers (storage), and CONFIG1/CONFIG2 (write-once options; COPD
// disables the COP).
#pragma once

#include <array>
#include <cstdint>

#include "hc08/periph.h"

namespace sim::sys {

class Cop;

class SimModule final : public Periph {
   public:
    SimModule() : Periph("sim") {
    }

    // offsets: $00..$1F = $FE00..$FE1F; $20 = CONFIG2 ($001E), $21 = CONFIG1 ($001F)
    static constexpr uint16_t kConfig2 = 0x20;
    static constexpr uint16_t kConfig1 = 0x21;

    void set_reset_cause(uint8_t srsr_bits) {
        srsr_ = srsr_bits;
    }

    void reset() override;
    uint8_t peek(uint16_t offset) override;

    Cop* cop = nullptr;

    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   protected:
    uint8_t rd(uint16_t offset) override;
    void wr(uint16_t offset, uint8_t value) override;

   private:
    uint8_t interrupt_status(int index) const;

    uint8_t srsr_ = 0x80;  // POR
    std::array<uint8_t, 0x20> regs_{};
    uint8_t config1_ = 0;
    uint8_t config2_ = 0;
    bool config1_written_ = false;
    bool config2_written_ = false;
};

}  // namespace sim::sys
