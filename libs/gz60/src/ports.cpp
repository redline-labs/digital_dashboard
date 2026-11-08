#include "gz60/ports.h"

#include "hc08/machine.h"
#include "emu/snapshot_io.h"

namespace sim::sys {

namespace {
constexpr DeviceRegister kRegisters[] = {{"PTA", 0x00},  {"PTB", 0x01},  {"PTC", 0x02},  {"PTD", 0x03}, {"DDRA", 0x04},
                                         {"DDRB", 0x05}, {"DDRC", 0x06}, {"DDRD", 0x07}, {"PTE", 0x08}, {"DDRE", 0x0C},
                                         {"PTF", 0x40},  {"PTG", 0x41},  {"DDRF", 0x44}, {"DDRG", 0x45}};
}

std::span<const DeviceRegister> Ports::registers() const {
    return kRegisters;
}

bool Ports::decode(uint16_t offset, size_t& port, int& kind) {
    switch (offset) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
            port = offset;
            kind = 0;
            return true;
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
            port = size_t{offset} - 4;
            kind = 1;
            return true;
        case 0x08:
            port = 4;
            kind = 0;
            return true;
        case 0x0C:
            port = 4;
            kind = 1;
            return true;
        case 0x0D:
            port = 0;
            kind = 2;
            return true;
        case 0x0E:
            port = 2;
            kind = 2;
            return true;
        case 0x0F:
            port = 3;
            kind = 2;
            return true;
        case 0x40:
        case 0x41:
            port = 5 + (size_t{offset} - 0x40);
            kind = 0;
            return true;
        case 0x44:
        case 0x45:
            port = 5 + (size_t{offset} - 0x44);
            kind = 1;
            return true;
        default:
            return false;
    }
}

void Ports::reset() {
    for (size_t p = 0; p < kPortCount; p++) {
        const uint8_t old = driven(Port(p));
        latch_[p] = 0;
        ddr_[p] = 0;
        pullup_[p] = 0;
        changed(p, old);
    }
}

void Ports::changed(size_t port, uint16_t old_driven) {
    if (on_drive && old_driven != 0xFFFF) {
        on_drive(Port(port), latch_[port], ddr_[port], m ? m->now() : 0);
    }
}

uint8_t Ports::rd(uint16_t offset) {
    size_t port = 0;
    int kind = 0;
    if (!decode(offset, port, kind)) {
        return 0;
    }
    switch (kind) {
        case 0:
            return uint8_t((latch_[port] & ddr_[port]) | (input_[port] & ~ddr_[port]));
        case 1:
            return ddr_[port];
        default:
            return pullup_[port];
    }
}

void Ports::wr(uint16_t offset, uint8_t value) {
    size_t port = 0;
    int kind = 0;
    if (!decode(offset, port, kind)) {
        return;
    }
    const uint8_t old = driven(Port(port));
    switch (kind) {
        case 0:
            latch_[port] = value;
            break;
        case 1:
            ddr_[port] = value;
            break;
        default:
            pullup_[port] = value;
            return;
    }
    changed(port, old);
}

void Ports::save(std::vector<uint8_t>& out) const {
    put_value(out, latch_);
    put_value(out, ddr_);
    put_value(out, pullup_);
    put_value(out, input_);
}

void Ports::load(const uint8_t*& in) {
    get_value(in, latch_);
    get_value(in, ddr_);
    get_value(in, pullup_);
    get_value(in, input_);
}

}  // namespace sim::sys
