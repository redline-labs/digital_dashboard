#include "gz60/soc.h"

namespace sim::sys {

Gz60::Gz60(MachineConfig config) : m(config) {
    // memory
    m.mem.add_ram(0x0040, 0x0400);
    m.mem.add_ram(0x0580, 0x0400);
    m.mem.add_flash(0x0462, 0x0500 - 0x0462);
    m.mem.add_flash(0x0980, 0x1B80 - 0x0980);
    m.mem.add_flash(0x1E20, 0x8000 - 0x1E20);
    m.mem.add_flash(0x8000, 0xFE00 - 0x8000);
    m.mem.add_rom(0xFE20, 0xFF80 - 0xFE20);
    m.mem.add_flash(0xFF80, 0x10000 - 0xFF80);

    // modules, in snapshot / event order
    sim.create("soc.sim");
    m.add(sim.shared());
    cgm.create("soc.cgm");
    m.add(cgm.shared());
    ports.create("soc.ports");
    m.add(ports.shared());
    cop.create("soc.cop");
    m.add(cop.shared());
    tim1.create("soc.tim1", "tim1", 2);
    m.add(tim1.shared());
    tim2.create("soc.tim2", "tim2", 6);
    m.add(tim2.shared());
    timebase.create("soc.timebase");
    m.add(timebase.shared());
    adc.create("soc.adc");
    m.add(adc.shared());
    flash.create("soc.flash");
    m.add(flash.shared());
    mscan.create("soc.mscan");
    m.add(mscan.shared());
    spi.create("soc.spi", "spi", uint16_t(3), std::vector<uint8_t>{0x00, 0x08, 0x00});
    m.add(spi.shared());
    sci.create("soc.sci", "sci", uint16_t(7), std::vector<uint8_t>{0x00, 0x00, 0x00, 0xC0});
    m.add(sci.shared());
    esci.create("soc.esci", "esci", uint16_t(3));
    m.add(esci.shared());
    kbi.create("soc.kbi", "kbi", uint16_t(2));
    m.add(kbi.shared());
    irq_control.create("soc.irq", "irq", uint16_t(1));
    m.add(irq_control.shared());
    io2.create("soc.io2", "io2", uint16_t(0x16));
    m.add(io2.shared());

    // register windows
    m.map(0x0000, 9, *ports);  // PTA..PTD, DDRA..DDRD, PTE
    m.map(0x0009, 3, *esci);   // SCPSC, SCIACTL, SCIADAT
    m.map(0x000C, 4, *ports, 0x0C);
    m.map(0x0010, 3, *spi);
    m.map(0x0013, 7, *sci);
    m.map(0x001A, 2, *kbi);
    m.map(0x001C, 1, *timebase);
    m.map(0x001D, 1, *irq_control);
    m.map(0x001E, 2, *sim, SimModule::kConfig2);
    m.map(0x0020, 11, *tim1);
    m.map(0x002B, 11, *tim2);
    m.map(0x0036, 5, *cgm);
    m.map(0x003C, 4, *adc);
    m.map(0x0440, 0x16, *io2);
    m.map(0x0440, 2, *ports, 0x40);
    m.map(0x0444, 2, *ports, 0x44);
    m.map(0x0456, 12, *tim2, 11);
    m.map(0x0500, 0x80, *mscan);
    m.map(0xFE00, 8, *sim);
    m.map(0xFE08, 1, *flash, FlashController::kFl2cr);
    m.map(0xFE09, 0x17, *sim, 0x09);
    m.map(0xFF88, 1, *flash, FlashController::kFl1cr);
    m.map(0xFFFF, 1, *cop);

    m.flash_write = [this](uint16_t addr, uint8_t value) { flash->array_write(addr, value); };
    sim->cop = cop.get();

    // interrupts
    tim1->overflow_irq = IrqLine(&m, irq::kTim1Overflow);
    tim1->channel_irq[0] = IrqLine(&m, irq::kTim1Ch0);
    tim1->channel_irq[1] = IrqLine(&m, irq::kTim1Ch1);
    tim2->overflow_irq = IrqLine(&m, irq::kTim2Overflow);
    tim2->channel_irq[0] = IrqLine(&m, irq::kTim2Ch0);
    tim2->channel_irq[1] = IrqLine(&m, irq::kTim2Ch1);
    tim2->channel_irq[2] = IrqLine(&m, irq::kTim2Ch2);
    tim2->channel_irq[3] = IrqLine(&m, irq::kTim2Ch3);
    tim2->channel_irq[4] = IrqLine(&m, irq::kTim2Ch4);
    tim2->channel_irq[5] = IrqLine(&m, irq::kTim2Ch5);
    timebase->irq = IrqLine(&m, irq::kTimebase);
    adc->irq = IrqLine(&m, irq::kAdc);
    mscan->wakeup_irq = IrqLine(&m, irq::kMscanWakeup);
    mscan->error_irq = IrqLine(&m, irq::kMscanError);
    mscan->receive_irq = IrqLine(&m, irq::kMscanReceive);
    mscan->transmit_irq = IrqLine(&m, irq::kMscanTransmit);
}

}  // namespace sim::sys
