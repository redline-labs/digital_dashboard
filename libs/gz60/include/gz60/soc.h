// The MC68HC908GZ60 as a whole: the machine with the GZ60's memory map, every module at its address and the
// interrupt wiring (MC68HC908GZ60 data sheet, "Memory Map" and the register tables).
//
//   $0000-$003F  I/O registers           $0040-$043F  RAM-1 (1 KB)
//   $0440-$0461  I/O (PTF/PTG, TIM2 2-5)  $0462-$04FF  FLASH-2
//   $0500-$057F  MSCAN08                 $0580-$097F  RAM-2 (1 KB)
//   $0980-$1B7F  FLASH-2                 $1E20-$7FFF  FLASH-2
//   $8000-$FDFF  FLASH-1                 $FE00-$FE1F  SIM, FL2CR, break
//   $FE20-$FF7F  monitor ROM             $FF80-$FFFF  FLASH-1 (FLxBPR, FL1CR at $FF88, vectors; COPCTL at $FFFF)
#pragma once

#include <memory>

#include "hc08/machine.h"
#include "gz60/adc.h"
#include "gz60/cgm.h"
#include "gz60/cop.h"
#include "gz60/flash.h"
#include "gz60/mscan.h"
#include "gz60/ports.h"
#include "gz60/sim_module.h"
#include "gz60/stub.h"
#include "gz60/tim.h"
#include "gz60/timebase.h"
#include "emu/part.h"

namespace sim::sys {

class Gz60 {
   public:
    explicit Gz60(MachineConfig config = {});
    Gz60(const Gz60&) = delete;
    Gz60& operator=(const Gz60&) = delete;

    Machine m;

    Part<SimModule> sim;
    Part<Cgm> cgm;
    Part<Ports> ports;
    Part<Cop> cop;
    Part<Tim> tim1;
    Part<Tim> tim2;
    Part<Timebase> timebase;
    Part<Adc> adc;
    Part<FlashController> flash;
    Part<Mscan> mscan;
    Part<StubDevice> spi;
    Part<StubDevice> sci;
    Part<StubDevice> esci;
    Part<StubDevice> kbi;
    Part<StubDevice> irq_control;
    Part<StubDevice> io2;
};

}  // namespace sim::sys
