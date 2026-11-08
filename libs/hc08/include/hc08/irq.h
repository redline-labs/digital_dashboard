// The GZ60's interrupt sources by vector (MC68HC908GZ60 data sheet, table "Vector addresses"). The source number is
// the vector's distance below the reset vector in words, so a lower number is a higher priority: the SIM's fixed
// priority (reset, SWI, IRQ, CGM, TIM1 ch0, ...).
#pragma once

#include <cstdint>

namespace sim::sys {

namespace irq {
inline constexpr int kIrq = 2;             // $FFFA
inline constexpr int kCgm = 3;             // $FFF8
inline constexpr int kTim1Ch0 = 4;         // $FFF6
inline constexpr int kTim1Ch1 = 5;         // $FFF4
inline constexpr int kTim1Overflow = 6;    // $FFF2
inline constexpr int kTim2Ch0 = 7;         // $FFF0
inline constexpr int kTim2Ch1 = 8;         // $FFEE
inline constexpr int kTim2Overflow = 9;    // $FFEC
inline constexpr int kSpiReceive = 10;     // $FFEA
inline constexpr int kSpiTransmit = 11;    // $FFE8
inline constexpr int kSciError = 12;       // $FFE6
inline constexpr int kSciReceive = 13;     // $FFE4
inline constexpr int kSciTransmit = 14;    // $FFE2
inline constexpr int kKeyboard = 15;       // $FFE0
inline constexpr int kAdc = 16;            // $FFDE
inline constexpr int kTimebase = 17;       // $FFDC
inline constexpr int kMscanWakeup = 18;    // $FFDA
inline constexpr int kMscanError = 19;     // $FFD8
inline constexpr int kMscanReceive = 20;   // $FFD6
inline constexpr int kMscanTransmit = 21;  // $FFD4
inline constexpr int kTim2Ch2 = 22;        // $FFD2
inline constexpr int kTim2Ch3 = 23;        // $FFD0
inline constexpr int kTim2Ch4 = 24;        // $FFCE
inline constexpr int kTim2Ch5 = 25;        // $FFCC
inline constexpr int kCount = 26;

inline constexpr uint16_t vector_address(int source) {
    return uint16_t(0xFFFE - 2 * source);
}

// The sources that bring the part out of STOP (the clocks they need keep running there).
inline constexpr uint32_t kStopWakeMask = (1u << kIrq) | (1u << kKeyboard) | (1u << kTimebase) | (1u << kMscanWakeup);
}  // namespace irq

}  // namespace sim::sys
