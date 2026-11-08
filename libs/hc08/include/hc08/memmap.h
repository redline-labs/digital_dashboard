// The 64 KB address space: which bytes are RAM, flash, a device register or nothing, and the storage of the plain
// ones.
//
// The CPU08 address space is small enough for flat tables: one byte of storage, one region tag and one device slot
// per address. RAM and flash read straight from `bytes`; a flash write is not a store (the flash controller decides:
// gz60/flash), and a device access goes to the device that owns the window, at its offset. The machine
// (hc08/machine.h) adds the device timing around the access; this class only answers "what is at this address".
#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace sim {

// A register window's owner. The offset is the device's own (a device with two windows, such as the ports, gives
// each window a different offset base).
class Device {
   public:
    virtual ~Device() = default;
    virtual uint8_t read(uint16_t offset) = 0;
    virtual void write(uint16_t offset, uint8_t value) = 0;
};

enum class Region : uint8_t {
    Unmapped,  // nothing: reads $00 (the GZ60's unimplemented locations), writes are dropped
    Ram,
    Flash,  // reads as storage; writes go to the flash controller (program/erase), never straight to storage
    Rom,    // the monitor ROM and reserved: reads as storage, writes dropped
    Device,
};

class MemoryMap {
   public:
    struct Slot {
        Device* device = nullptr;
        uint16_t base = 0;    // the window's first address
        uint16_t offset = 0;  // the device offset of that address
    };

    MemoryMap() {
        bytes.fill(0);
        region_.fill(Region::Unmapped);
        slot_.fill(0);
        slots_.push_back(Slot{});  // slot 0: none
    }

    void add_ram(uint16_t base, uint32_t size) {
        set(base, size, Region::Ram, 0);
    }

    // Flash reads as erased ($FF) until an image is loaded.
    void add_flash(uint16_t base, uint32_t size) {
        set(base, size, Region::Flash, 0);
        for (uint32_t k = 0; k < size; k++) {
            bytes[base + k] = 0xFF;
        }
    }

    void add_rom(uint16_t base, uint32_t size) {
        set(base, size, Region::Rom, 0);
    }

    // `device` (not owned) answers [base, base + size) at offsets offset.. upward.
    void add_device(uint16_t base, uint32_t size, Device* device, uint16_t offset = 0) {
        if (slots_.size() >= 255) {
            throw std::runtime_error("memory map: too many device windows");
        }

        slots_.push_back(Slot{device, base, offset});
        set(base, size, Region::Device, uint8_t(slots_.size() - 1));
    }

    [[gnu::always_inline]] Region region(uint16_t addr) const {
        return region_[addr];
    }

    [[gnu::always_inline]] const Slot& slot(uint16_t addr) const {
        return slots_[slot_[addr]];
    }

    // the device offset of `addr` (a Device region)
    [[gnu::always_inline]] uint16_t device_offset(uint16_t addr) const {
        const Slot& s = slot(addr);
        return uint16_t(addr - s.base + s.offset);
    }

    // The region tags, one byte per address (the JIT's generated accesses read them).
    const uint8_t* region_bytes() const {
        return reinterpret_cast<const uint8_t*>(region_.data());
    }

    // Storage: RAM, flash and ROM contents (device addresses' bytes are unused).
    std::array<uint8_t, 65536> bytes;

   private:
    void set(uint16_t base, uint32_t size, Region region, uint8_t slot) {
        if (base + size > 0x10000) {
            throw std::runtime_error("memory map: window past $FFFF at $" + std::to_string(base));
        }

        for (uint32_t k = 0; k < size; k++) {
            region_[base + k] = region;
            slot_[base + k] = slot;
        }
    }

    std::array<Region, 65536> region_;
    std::array<uint8_t, 65536> slot_;
    std::vector<Slot> slots_;
};

}  // namespace sim
