// A peripheral: a timed device (emu/timed.h) with a register window in the CPU08's address space, and an interrupt
// source or more.
//
// Devices are lazy, as in mk60e5: a Timed device says when its next event is (next_event, in ticks) and catches up to
// a time when asked (sync). The machine syncs a device before every register access and at its events, so a device
// never runs a clock of its own. A device with no register window (the PDM board, the stimulus) is an emu::Timed and
// is scheduled the same way.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "emu/common.h"
#include "emu/timed.h"
#include "hc08/memmap.h"

namespace sim::sys {

class Machine;

// A named register, for the debugger's DEVICE.NAME reads and the agent's register dumps.
struct DeviceRegister {
    std::string_view name;
    uint16_t offset;
};

class Periph : public Timed, public Device {
   public:
    explicit Periph(std::string name, Timing timing = Timing::Passive) : Timed(std::move(name), timing) {
    }

    uint8_t read(uint16_t offset) final {
        return rd(offset);
    }

    void write(uint16_t offset, uint8_t value) final {
        wr(offset, value);
    }

    // The register's value without side effects (no flag clearing): the debugger's view. Defaults to rd(), which is
    // right for the devices whose reads have no side effects.
    virtual uint8_t peek(uint16_t offset) {
        return rd(offset);
    }

    // The bus clock changed (the PLL was switched): the device was synced to `now` at the old rate first.
    virtual void bus_clock_changed(uint64_t now) {
        (void)now;
    }

    // The CPU entered (true) or left STOP at `now`: devices clocked by the bus freeze in between.
    virtual void stop_mode(bool entering, uint64_t now) {
        (void)entering;
        (void)now;
    }

    virtual std::span<const DeviceRegister> registers() const {
        return {};
    }

    Machine* m = nullptr;

   protected:
    virtual uint8_t rd(uint16_t offset) = 0;
    virtual void wr(uint16_t offset, uint8_t value) = 0;
};

}  // namespace sim::sys
