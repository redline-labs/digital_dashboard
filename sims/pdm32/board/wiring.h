// How the PDM32's connector reaches the GZ60's pins. Both tables are the
// board's wiring as the v1.91U application encodes it; tests/test_board.cpp checks them against the image when the
// firmware is available.
//
//   outputs  each output's high-side switch gate is one port pin:
//              O1 PTD6  O2 PTD7  O3-O8 PTE0-5  O9-O12 PTF4-7  O13 PTD4  O14 PTD5  O15-O18 PTF0-3
//              O19-O24 PTA2-7  O25-O29 PTC2-6  O30-O32 PTD1-3
//   sensing  external 8:1 analog multiplexers addressed by PTB0, PTB1, PTB4 feed the ADC channels; the firmware's
//            ADC scan says which (channel, address) it stores where, which names the signal: battery (AD2), the 9.5 V rail (AD7), board temperature
//            (AD3), output voltages (AD16-19), output currents (AD20-23, same addresses as the voltages), inputs
//            (AD5-7).
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "gz60/ports.h"

namespace sim::sys::pdm32 {

struct Pin {
    Port port;
    uint8_t bit;
};

// output k (0-based) -> its gate pin
extern const std::array<Pin, 32> kOutputPins;

enum class Sense : uint8_t { None, Battery, Rail9V5, BoardTemp, OutputVoltage, OutputCurrent, InputVoltage };

struct SenseSource {
    Sense kind = Sense::None;
    uint8_t index = 0;  // output or input, 0-based
};

// The v1.91U scan table, byte for byte.
extern const std::array<uint8_t, 360> kAdcScanTable;

// What the ADC sees on `channel` with the multiplexer address `mux` (PTB & $13).
SenseSource sense_source(int channel, uint8_t mux);

}  // namespace sim::sys::pdm32
