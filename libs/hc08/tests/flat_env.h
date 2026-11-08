// A bare CPU environment for the core's tests: 64 KB of RAM, no devices, an interrupt line set by hand.
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>

#include "hc08/cpu.h"

namespace sim::test {

struct FlatEnv {
    std::array<uint8_t, 65536> mem{};
    uint64_t limit = ~uint64_t(0);
    uint32_t irq_source_vector = 0;  // 0: no interrupt; else the vector address
    bool pin = true;
    uint64_t reads = 0;
    uint64_t writes = 0;

    uint8_t fetch(uint16_t a) const {
        return mem[a];
    }
    uint8_t read(uint16_t a) {
        reads++;
        return mem[a];
    }
    void write(uint16_t a, uint8_t v) {
        writes++;
        mem[a] = v;
    }
    uint64_t cycle_limit() const {
        return limit;
    }
    bool irq_pending() const {
        return irq_source_vector != 0;
    }
    uint16_t irq_vector() const {
        return uint16_t(irq_source_vector);
    }
    bool watching() const {
        return false;
    }
    bool pc_watched(uint16_t) const {
        return false;
    }
    bool irq_pin() const {
        return pin;
    }

    void load(uint16_t addr, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            mem[addr++] = b;
        }
    }
};

// Executes exactly one instruction at s.pc (no interrupt check).
inline Stop step(CpuState& s, FlatEnv& env) {
    return execute_insn(s, env, decode(s.pc, [&env](uint16_t a) { return env.fetch(a); }));
}

}  // namespace sim::test
