// A peripheral: a device with a register window, a time base and an interrupt source or more.
//
// Devices are lazy, as in mk60e5: a Timed device says when its next event is (next_event, in ticks) and catches up to
// a time when asked (sync). The machine syncs a device before every register access and at its events, so a device
// never runs a clock of its own. An Input device (the stimulus) is synced before the others at the same tick.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "emu/common.h"
#include "hc08/memmap.h"

namespace sim::sys {

class Machine;

enum class Timing : uint8_t { Passive, Timed, Input };

// A named register, for the debugger's DEVICE.NAME reads and the agent's register dumps.
struct DeviceRegister {
    std::string_view name;
    uint16_t offset;
};

class Periph : public Device {
   public:
    explicit Periph(std::string name, Timing timing = Timing::Passive) : name_(std::move(name)), timing_(timing) {
    }

    const std::string& name() const {
        return name_;
    }

    Timing timing() const {
        return timing_;
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

    // The tick of the next event (kNever: none). Must be after the time of the last sync once that sync is done.
    virtual uint64_t next_event() const {
        return kNever;
    }

    // Catch up to `now` (ticks): raise the flags and interrupts due by then.
    virtual void sync(uint64_t now) {
        (void)now;
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

    // A reset (power-on, COP, illegal opcode): registers to their reset values.
    virtual void reset() {
    }

    virtual void save(std::vector<uint8_t>& out) const {
        (void)out;
    }

    virtual void load(const uint8_t*& in) {
        (void)in;
    }

    virtual std::span<const DeviceRegister> registers() const {
        return {};
    }

    Machine* m = nullptr;

   protected:
    virtual uint8_t rd(uint16_t offset) = 0;
    virtual void wr(uint16_t offset, uint8_t value) = 0;

   private:
    std::string name_;
    Timing timing_;
};

}  // namespace sim::sys
