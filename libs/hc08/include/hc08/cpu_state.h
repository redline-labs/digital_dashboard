// The CPU08's programmer-visible state and the result of a run of the core.
#pragma once

#include <cstdint>

#include "emu/common.h"

namespace sim {

// Condition code register bits (CCR = V 1 1 H I N Z C).
namespace ccr {
inline constexpr uint8_t kC = 0x01;
inline constexpr uint8_t kZ = 0x02;
inline constexpr uint8_t kN = 0x04;
inline constexpr uint8_t kI = 0x08;
inline constexpr uint8_t kH = 0x10;
inline constexpr uint8_t kV = 0x80;
inline constexpr uint8_t kOnes = 0x60;  // bits 6 and 5 always read 1
}  // namespace ccr

struct CpuState {
    uint8_t a = 0;
    uint16_t hx = 0;  // H:X, the 16-bit index register
    uint16_t sp = 0x00FF;
    uint16_t pc = 0;
    uint8_t ccr = ccr::kOnes | ccr::kI;

    // Bus cycles executed since reset: the core's only clock. The machine converts them to ticks.
    uint64_t cycles = 0;

    // CLI (and TAP clearing I) holds interrupts off for one more instruction.
    bool irq_inhibit = false;

    // A breakpoint at this pc is passed once (the resume of a stop at a breakpoint).
    int32_t skip_watch_pc = -1;

    uint8_t h() const {
        return uint8_t(hx >> 8);
    }

    uint8_t x() const {
        return uint8_t(hx);
    }

    void set_h(uint8_t value) {
        hx = uint16_t((uint16_t(value) << 8) | (hx & 0xFF));
    }

    void set_x(uint8_t value) {
        hx = uint16_t((hx & 0xFF00) | value);
    }

    bool flag(uint8_t mask) const {
        return (ccr & mask) != 0;
    }

    void set_flag(uint8_t mask, bool on) {
        ccr = uint8_t(on ? (ccr | mask) : (ccr & ~mask));
    }
};

// Why the core returned. A stop is a value, never an exception.
enum class StopKind : uint8_t {
    None,      // (internal: the instruction completed)
    Limit,     // the cycle limit was reached (the quantum ended)
    Wait,      // WAIT executed: the CPU sleeps until an interrupt (pc is past the WAIT)
    StopMode,  // STOP executed: the CPU and the bus clocks stop until a stop-mode interrupt (pc is past the STOP)
    Illegal,   // an opcode the CPU08 does not implement (pc at the opcode): the real part resets (ILOP)
    Watch,     // a watched pc was reached (pc at it, not executed)
    Break,     // a data watch fired or a break was requested: the run stopped after the instruction
    Exit,      // request_exit(): the run was asked to end (another thread, a shutdown)
};

struct Stop {
    StopKind kind = StopKind::None;
    uint16_t pc = 0;
    uint16_t detail = 0;  // Illegal: the opcode (with the $9E prefix in the high byte)
};

}  // namespace sim
