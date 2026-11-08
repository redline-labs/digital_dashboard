// The output current calibration (layout::kCurrentCal):
// per output, how the ADC ISR turns a raw current-sense count into the reported count (0.5 A/bit O1-8, 0.2 A/bit
// O9-32). The block is the factory's, one per unit; the simulator compiles in a real unit's (current_cal_dumped.h).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pdm32/config/layout.h"

namespace pdm::config {

struct OutputCurrentCal {
    std::array<uint8_t, 32> low_lut{};  // raw < 32: the calibrated count is low_lut[raw]
    uint16_t gain = 128;                // raw >= 32: t = raw * gain + offset (16 bits);
    int16_t offset = 0;                 //            $FF if t bit 15 is set, else t >> 7

    // the firmware's calibration of one raw count
    constexpr uint8_t apply(uint8_t raw) const {
        if (raw < 32) {
            return low_lut[raw];
        }
        const uint16_t t = uint16_t(raw * gain + offset);
        return (t & 0x8000) ? 0xFF : uint8_t(t >> 7);
    }

    // The sense hardware this calibration was made for: for each calibrated count, the smallest raw count that
    // calibrates to at least it (255 when none does). The board's current sense produces raw counts through it.
    constexpr std::array<uint8_t, 256> raw_for_count() const {
        std::array<uint8_t, 256> raw{};
        size_t r = 0;
        for (size_t c = 0; c < 256; c++) {
            while (r < 255 && apply(uint8_t(r)) < c) {
                r++;
            }
            raw[c] = uint8_t(r);
        }
        return raw;
    }
};

struct CurrentCal {
    std::array<OutputCurrentCal, 32> outputs{};
};

// raw count = calibrated count on every output
constexpr CurrentCal unity_current_cal() {
    CurrentCal cal;
    for (OutputCurrentCal& o : cal.outputs) {
        for (size_t i = 0; i < 32; i++) {
            o.low_lut[i] = uint8_t(i);
        }
    }
    return cal;
}

// The block's bytes (format 1, length, records, CRC), to be placed at layout::kCurrentCal.
// The block's size: a 4-byte header, 36 bytes per output, the CRC.
inline constexpr size_t kCurrentCalBlockBytes = 4 + layout::kOutputs * layout::kCalEntryBytes + 2;

std::vector<uint8_t> encode_current_cal(const CurrentCal& cal);

// Parses a block (as dumped or as encoded). Throws std::runtime_error on a bad header, length or CRC.
CurrentCal decode_current_cal(std::span<const uint8_t> block);

// A compiled-in calibration by name: "dumped" (the default, from a real unit) or "unity". Throws on an unknown name.
const CurrentCal& builtin_current_cal(const std::string& name);

inline constexpr const char* kDefaultCurrentCal = "dumped";

}  // namespace pdm::config
