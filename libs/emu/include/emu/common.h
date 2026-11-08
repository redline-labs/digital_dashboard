// Common types and helpers for the CPU08 simulator.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sim {

// A time that never comes (ticks or cycles): no event scheduled, nothing due, none known yet.
inline constexpr uint64_t kNever = ~uint64_t(0);

// The CPU08 is big-endian: a 16-bit value in memory is its high byte, then its low byte.
[[gnu::always_inline]] inline uint16_t load_be16(const uint8_t* p) {
    return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}

[[gnu::always_inline]] inline void store_be16(uint8_t* p, uint16_t value) {
    p[0] = uint8_t(value >> 8);
    p[1] = uint8_t(value);
}

// Sign-extend a byte (branch offsets, AIS/AIX operands).
[[gnu::always_inline]] inline int16_t sext8(uint8_t value) {
    return int16_t(int8_t(value));
}

}  // namespace sim
