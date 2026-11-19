// A bare core environment for the e200's tests: 1 MB of memory at physical 0, nothing else (a bus error), the
// interrupt inputs set by hand. Translation is through the core's MMU when `mmu` is set, else effective = physical.
#pragma once

#include <cstdint>
#include <vector>

#include "e200/assembler.h"
#include "e200/cpu.h"

namespace sim::e200::test {

struct FlatEnv {
    static constexpr uint32_t kSize = 1u << 20;

    CpuState* state = nullptr;
    std::vector<uint8_t> mem = std::vector<uint8_t>(kSize);
    bool mmu = false;
    uint64_t limit = ~uint64_t(0);
    bool external = false;
    bool critical = false;
    uint32_t translation_changes = 0;
    std::vector<Vector> interrupts;

    Fault phys(uint32_t ea, Access access, uint32_t* pa) const {
        if (!mmu) {
            *pa = ea;
            return Fault::None;
        }
        return mmu_translate(*state, ea, access, pa);
    }

    Fault fetch(uint32_t ea, uint32_t* word) {
        return load_as(ea, 4, word, Access::Execute);
    }
    Fault load(uint32_t ea, unsigned size, uint32_t* value) {
        return load_as(ea, size, value, Access::Read);
    }
    Fault load_as(uint32_t ea, unsigned size, uint32_t* value, Access access) {
        uint32_t pa = 0;
        if (const Fault f = phys(ea, access, &pa); f != Fault::None) {
            return f;
        }
        if (pa > kSize - size) {
            return Fault::Bus;
        }
        uint32_t v = 0;
        for (unsigned i = 0; i < size; ++i) {
            v = (v << 8) | mem[pa + i];
        }
        *value = v;
        return Fault::None;
    }
    Fault store(uint32_t ea, unsigned size, uint32_t value) {
        uint32_t pa = 0;
        if (const Fault f = phys(ea, Access::Write, &pa); f != Fault::None) {
            return f;
        }
        if (pa > kSize - size) {
            return Fault::Bus;
        }
        for (unsigned i = 0; i < size; ++i) {
            mem[pa + i] = uint8_t(value >> (8 * (size - 1 - i)));
        }
        return Fault::None;
    }
    void translation_changed() {
        ++translation_changes;
    }
    uint64_t cycle_limit() const {
        return limit;
    }
    bool external_pending() const {
        return external;
    }
    bool critical_pending() const {
        return critical;
    }
    bool watching() const {
        return false;
    }
    bool pc_watched(uint32_t) const {
        return false;
    }
    Stop on_interrupt(Vector v, uint32_t) {
        interrupts.push_back(v);
        return Stop{};
    }

    void put(const Asm& a) {
        const std::vector<uint8_t> bytes = a.bytes();
        std::copy(bytes.begin(), bytes.end(), mem.begin() + a.origin());
    }
    void put32(uint32_t pa, uint32_t v) {
        for (unsigned i = 0; i < 4; ++i) {
            mem[pa + i] = uint8_t(v >> (24 - 8 * i));
        }
    }
    uint32_t get32(uint32_t pa) const {
        return (uint32_t(mem[pa]) << 24) | (uint32_t(mem[pa + 1]) << 16) | (uint32_t(mem[pa + 2]) << 8) | mem[pa + 3];
    }
};

// A supervisor core at `pc`, interrupts off.
inline CpuState core_at(uint32_t pc) {
    CpuState s;
    s.pc = pc;
    return s;
}

// Runs from s.pc for `cycles` instructions (or until a stop).
inline Stop run_for(CpuState& s, FlatEnv& env, uint64_t cycles) {
    env.state = &s;
    env.limit = s.cycles + cycles;
    return interpret(s, env);
}

}  // namespace sim::e200::test
