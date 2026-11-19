// eSCI, the MPC55xx serial port (MPC5554 RM, chapter 19), as a console: what software writes to the data register
// goes to `on_byte` at once, and the status register always says the transmitter is empty and done. Reception,
// baud rates, LIN and the interrupts are not modelled.
//
// Registers: CR1 0x00, CR2 0x04, DR 0x06 (16 bits; the low byte is the data), SR 0x08, LCR 0x0C.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "e200/machine.h"
#include "mpc55xx/registers.h"

namespace sim::mpc55xx {

class Esci final : public e200::BusDevice {
   public:
    explicit Esci(std::string name) : BusDevice(std::move(name)), regs_(0x20) {
    }

    std::function<void(uint8_t)> on_byte;

    uint32_t read(uint32_t offset, unsigned size) override {
        if ((offset & ~3u) == 0x08) {
            RegisterFile word(4);
            word.set32(0, 0xC0000000);  // TDRE, TC
            return word.get(offset & 3, size);
        }
        return regs_.get(offset, size);
    }
    void write(uint32_t offset, unsigned size, uint32_t value) override {
        // the data byte is offset 7: a 16-bit write at 6, an 8-bit write at 7, or a 32-bit write at 4
        if (offset <= 7 && offset + size > 7 && on_byte) {
            on_byte(uint8_t(value >> (8 * (offset + size - 1 - 7))));
        }
        regs_.set(offset, size, value);
    }
    void reset() override {
        regs_.clear();
    }

   private:
    RegisterFile regs_;
};

}  // namespace sim::mpc55xx
