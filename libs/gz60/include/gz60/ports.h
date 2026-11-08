// The parallel ports A..G: data latches, data direction, pull-up enables. A pin driven as an output reads its latch;
// an input pin reads the level the board puts on it (set_input). Every change of what the MCU drives (latch or
// direction) is told to the board (on_drive), which is how the output switches see their gates.
//
// Offsets: $00..$0F are the registers at $0000..$000F (PTA..PTE, DDRA..DDRE, PTxPUE); $40..$45 are $0440..$0445
// (PTF, PTG, DDRF, DDRG).
#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "hc08/periph.h"

namespace sim::sys {

enum class Port : uint8_t { A, B, C, D, E, F, G };
inline constexpr size_t kPortCount = 7;

class Ports final : public Periph {
   public:
    Ports() : Periph("ports") {
    }

    // What the MCU drives on a port: `level` on the pins whose direction bit is set in `ddr`.
    std::function<void(Port port, uint8_t level, uint8_t ddr, uint64_t now)> on_drive;

    // The board's level on a port's pins (read where the direction is input).
    void set_input(Port port, uint8_t level) {
        input_[size_t(port)] = level;
    }

    uint8_t latch(Port port) const {
        return latch_[size_t(port)];
    }
    uint8_t ddr(Port port) const {
        return ddr_[size_t(port)];
    }
    // the levels the MCU drives (0 where the pin is an input)
    uint8_t driven(Port port) const {
        return uint8_t(latch_[size_t(port)] & ddr_[size_t(port)]);
    }

    void reset() override;
    std::span<const DeviceRegister> registers() const override;

    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   protected:
    uint8_t rd(uint16_t offset) override;
    void wr(uint16_t offset, uint8_t value) override;

   private:
    // which port and register an offset is: 0 = data, 1 = direction, 2 = pull-up; -1: none
    static bool decode(uint16_t offset, size_t& port, int& kind);
    void changed(size_t port, uint16_t old_driven);

    std::array<uint8_t, kPortCount> latch_{};
    std::array<uint8_t, kPortCount> ddr_{};
    std::array<uint8_t, kPortCount> pullup_{};
    std::array<uint8_t, kPortCount> input_{};
};

}  // namespace sim::sys
