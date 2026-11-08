// The two flash arrays' controllers (FL1CR at $FF88 for FLASH-1, FL2CR at $FE08 for FLASH-2; MC68HC908GZ60 data
// sheet, "FLASH Memory"). Program: set PGM, write to the row (address latch), set HVEN, write the bytes (each write
// ANDs into the cell: programming only clears bits), clear PGM, clear HVEN. Erase: set ERASE (and MASS for the whole
// array), write to the page (latch), set HVEN: the page (128 bytes) or the array becomes $FF. The timing (tPROG,
// tERASE) is not modelled: the firmware's delay loops pass the time anyway.
//
// Block protection (FLxBPR) is not enforced: a write into a protected range is logged.
#pragma once

#include <array>
#include <optional>
#include <cstdint>

#include "hc08/periph.h"

namespace sim::sys {

class FlashController final : public Periph {
   public:
    FlashController() : Periph("flash") {
    }

    // offsets: 0 = FL1CR ($FF88), 1 = FL2CR ($FE08)
    static constexpr uint16_t kFl1cr = 0;
    static constexpr uint16_t kFl2cr = 1;
    static constexpr uint32_t kPageBytes = 128;

    // Which array an address belongs to: 0 FLASH-1, 1 FLASH-2, or neither.
    static std::optional<size_t> array_of(uint16_t addr);

    // The CPU wrote `value` to flash at `addr` (Machine::flash_write).
    void array_write(uint16_t addr, uint8_t value);

    uint64_t bytes_programmed() const {
        return programmed_;
    }
    uint64_t erases() const {
        return erases_;
    }

    void reset() override;
    std::span<const DeviceRegister> registers() const override;

    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   protected:
    uint8_t rd(uint16_t offset) override;
    void wr(uint16_t offset, uint8_t value) override;

   private:
    void erase(size_t array, uint16_t latched, bool mass);

    std::array<uint8_t, 2> cr_{};
    std::array<int32_t, 2> latch_{-1, -1};
    uint64_t programmed_ = 0;
    uint64_t erases_ = 0;
};

}  // namespace sim::sys
