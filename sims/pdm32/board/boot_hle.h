// The stand-in for MoTeC's bootloader, which is not in pdm.hex: what the application relies on it having done, done
// directly.
//
//   vectors   point at the application's redirect table, one jump per vector in vector order, IRQ and SWI included;
//             reset at the application's entry (layout::kAppEntry, layout::kVectorRedirects)
//   clocks    the PLL at the 32 MHz VCO / 8 MHz bus the application assumes (it only reprograms it after sleep)
//   reset     SRSR carries the cause (POR on the first start)
//   hardware  the hardware-revision byte = 0; FL1BPR/FL2BPR erased (nothing protected)
//
// What is not reproduced: the bootloader's own CAN protocol (firmware update), its checks of the application image,
// and its CONFIG1/CONFIG2 writes. The model's defaults match what the application depends on (the COP on with the
// long timeout, STOP enabled, MSCAN on, the timebase dividers of TMBCLKSEL=0).
#pragma once

#include "pdm32/board/pdm32.h"

namespace sim::sys {

// Installs the vectors and the reset hook on the board's machine. Call after the application image is loaded.
void install_boot_stand_in(Pdm32Board& board);

}  // namespace sim::sys
