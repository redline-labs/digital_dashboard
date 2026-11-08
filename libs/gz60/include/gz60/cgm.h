// The clock generator module (PLL): PCTL, PBWC, PMSH/PMSL (N), PMRS (L) at $0036..$003A ($003B is reserved). The bus
// clock follows BCS: CGMVCLK / 4 with it, CGMXCLK / 4 without (gz60/clocks.h). LOCK reads set as soon as the PLL is on
// (the acquisition time is not modelled: the firmware polls LOCK before setting BCS).
#pragma once

#include <array>
#include <cstdint>

#include "hc08/periph.h"

namespace sim::sys {

class Cgm final : public Periph {
   public:
    Cgm() : Periph("cgm") {
    }

    static constexpr uint16_t kPctl = 0;
    static constexpr uint16_t kPbwc = 1;
    static constexpr uint16_t kPmsh = 2;
    static constexpr uint16_t kPmsl = 3;
    static constexpr uint16_t kPmrs = 4;

    // The bus clock the registers select, in Hz.
    uint64_t bus_hz() const;

    // The bootloader's setting (32 MHz VCO, 8 MHz bus), applied by the boot stand-in (board/boot_hle).
    void set_bootloader_state();

    void reset() override;
    std::span<const DeviceRegister> registers() const override;

    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   protected:
    uint8_t rd(uint16_t offset) override;
    void wr(uint16_t offset, uint8_t value) override;

   private:
    void apply();
    std::array<uint8_t, 5> regs_{};
};

}  // namespace sim::sys
