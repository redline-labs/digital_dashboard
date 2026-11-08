// The GZ60's clock tree as the devices need it (MC68HC908GZ60 data sheet, "Clock Generator Module").
//
//   CGMXCLK  the crystal: 4 MHz on the PDM. The bootloader and the application program N=8, E=2 (the 16-32 MHz VCO
//            range) and L=$70 (centre 112 * 2^2 * 71.4 kHz = 32 MHz), so the VCO runs at 32 MHz = N * 4 MHz. A unit's
//            CAN capture confirms the 8 MHz bus: its data stream runs off the 1 ms TIM1 tick (8000 bus cycles).
//   CGMVCLK  N * CGMXCLK (the PLL; E only selects the VCO's range, and the GZ60 has no reference divider)
//   CGMOUT   CGMVCLK / 2 with BCS set, CGMXCLK / 2 without; twice the bus clock either way
//   bus      CGMOUT / 2: 8 MHz on the PLL, 1 MHz from the crystal (the firmware's sleep)
#pragma once

#include <cstdint>

namespace sim::sys {

inline constexpr uint64_t kCrystalHz = 4'000'000;

}  // namespace sim::sys
