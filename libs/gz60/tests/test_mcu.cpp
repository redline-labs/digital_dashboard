// The GZ60 modules through their registers: TIM, ADC, CGM, flash program/erase, timebase, COP, ports.
#include "gz60/soc.h"
#include "mctest/test.h"

using namespace sim;
using namespace sim::sys;

namespace {

constexpr uint64_t kMs = 32'000;  // ticks

// A GZ60 whose CPU idles in a WAIT loop at $8000 with interrupts masked off (so device flags just accumulate).
struct Bench {
    Gz60 soc;
    Bench() {
        soc.m.mem.bytes[0xFFFE] = 0x80;
        soc.m.mem.bytes[0xFFFF] = 0x00;
        soc.m.mem.bytes[0x8000] = 0x20;  // BRA *
        soc.m.mem.bytes[0x8001] = 0xFE;
        soc.m.reset(ResetCause::PowerOn);
        soc.cgm->set_bootloader_state();
        soc.cop->set_enabled(false);
    }
    void run_ms(double ms) {
        soc.m.run_until(soc.m.now() + uint64_t(ms * kMs));
    }
};

}  // namespace

TEST(mcu_cgm_bootloader_state_gives_8mhz_bus) {
    Bench b;
    CHECK_EQ(b.soc.m.bus_hz(), 8'000'000);
    CHECK_EQ(b.soc.m.ticks_per_cycle(), 4);
    b.soc.m.poke(0x0036, 0x20);  // PLL on, BCS off: the crystal / 4
    CHECK_EQ(b.soc.m.bus_hz(), 1'000'000);
    CHECK_EQ(b.soc.m.ticks_per_cycle(), 32);
    CHECK((b.soc.m.peek(0x0037) & 0x40) != 0);  // LOCK
}

TEST(mcu_tim1_1ms_tick) {
    Bench b;
    b.soc.m.poke(0x0020, 0x30);  // stop + reset
    b.soc.m.poke(0x0023, 0x1F);
    b.soc.m.poke(0x0024, 0x40);  // modulo 8000
    b.soc.m.poke(0x0020, 0x40);  // TOIE, run, /1
    b.run_ms(10.01);
    // 8001 counts per overflow at 8 MHz: 10 overflows in 10.01 ms
    CHECK_EQ(b.soc.tim1->overflows(), 10);
    CHECK((b.soc.m.irq_lines() & (1u << irq::kTim1Overflow)) != 0);
    // clearing TOF: read it set, write 0
    (void)b.soc.m.read(0x0020);
    b.soc.m.write(0x0020, 0x40);
    CHECK((b.soc.m.irq_lines() & (1u << irq::kTim1Overflow)) == 0);
}

TEST(mcu_adc_samples_at_start_and_interrupts_after_17_clocks) {
    Bench b;
    int sampled_channel = -1;
    b.soc.adc->sample = [&](int channel, uint64_t) {
        sampled_channel = channel;
        return 2.5;
    };
    b.soc.m.poke(0x003F, 0x70);  // bus / 8, 8-bit
    b.soc.m.poke(0x003C, 0x45);  // AIEN, channel 5
    CHECK_EQ(sampled_channel, 5);
    b.run_ms(0.016);  // 16 us: not done (17 x 1 us)
    CHECK((b.soc.m.irq_lines() & (1u << irq::kAdc)) == 0);
    b.run_ms(0.002);
    CHECK((b.soc.m.irq_lines() & (1u << irq::kAdc)) != 0);
    CHECK_EQ(b.soc.m.read(0x003E), 128);                    // 2.5 V of 5 V: 512 >> 2
    CHECK((b.soc.m.irq_lines() & (1u << irq::kAdc)) == 0);  // reading ADRL cleared COCO
}

TEST(mcu_flash2_program_and_page_erase) {
    Bench b;
    const uint16_t addr = 0x1E20;
    // program $5A
    b.soc.m.write(0xFE08, 0x01);  // PGM
    b.soc.m.write(addr, 0x00);    // latch
    b.soc.m.write(0xFE08, 0x09);  // HVEN
    b.soc.m.write(addr, 0x5A);
    b.soc.m.write(addr + 1, 0xF0);
    b.soc.m.write(0xFE08, 0x08);
    b.soc.m.write(0xFE08, 0x00);
    CHECK_EQ(b.soc.m.mem.bytes[addr], 0x5A);
    CHECK_EQ(b.soc.m.mem.bytes[addr + 1], 0xF0);
    // programming only clears bits
    b.soc.m.write(0xFE08, 0x01);
    b.soc.m.write(addr, 0x00);
    b.soc.m.write(0xFE08, 0x09);
    b.soc.m.write(addr, 0xFF);
    b.soc.m.write(0xFE08, 0x00);
    CHECK_EQ(b.soc.m.mem.bytes[addr], 0x5A);
    // page erase
    b.soc.m.write(0xFE08, 0x02);
    b.soc.m.write(addr, 0x00);
    b.soc.m.write(0xFE08, 0x0A);
    b.soc.m.write(0xFE08, 0x00);
    CHECK_EQ(b.soc.m.mem.bytes[addr], 0xFF);
    CHECK_EQ(b.soc.flash->erases(), 1);
    // an idle write does nothing
    b.soc.m.write(addr, 0x00);
    CHECK_EQ(b.soc.m.mem.bytes[addr], 0xFF);
}

TEST(mcu_timebase_period_and_ack) {
    Bench b;
    b.soc.m.poke(0x001C, 0x1C);  // TBR=1 (/8192), TACK, TBIE
    b.soc.m.poke(0x001C, 0x1E);  // + TBON
    b.run_ms(2.0);
    CHECK((b.soc.m.irq_lines() & (1u << irq::kTimebase)) == 0);
    b.run_ms(0.1);  // 2.048 ms at the 4 MHz crystal
    CHECK((b.soc.m.irq_lines() & (1u << irq::kTimebase)) != 0);
    b.soc.m.poke(0x001C, 0x1E | 0x08);
    CHECK((b.soc.m.irq_lines() & (1u << irq::kTimebase)) == 0);
}

TEST(mcu_cop_resets_without_feeding) {
    Bench b;
    b.soc.cop->set_enabled(true);
    b.run_ms(65);
    CHECK_EQ(b.soc.m.reset_count(), 1);  // the bench's own reset
    b.run_ms(1);
    CHECK_EQ(b.soc.m.reset_count(), 2);           // 65.532 ms
    CHECK_EQ(b.soc.m.read(0xFE01) & 0x20, 0x00);  // (SRSR is set by the boot stand-in, not by the machine alone)
}

TEST(mcu_ports_drive_and_inputs) {
    Bench b;
    Port seen = Port::G;
    uint8_t level = 0;
    uint8_t ddr = 0;
    b.soc.ports->on_drive = [&](Port p, uint8_t l, uint8_t d, uint64_t) {
        seen = p;
        level = l;
        ddr = d;
    };
    b.soc.m.write(0x0444, 0xFF);  // DDRF
    b.soc.m.write(0x0440, 0x10);  // PTF4 (output 9)
    CHECK(seen == Port::F);
    CHECK_EQ(level, 0x10);
    CHECK_EQ(ddr, 0xFF);
    b.soc.ports->set_input(Port::B, 0xA5);
    b.soc.m.write(0x0005, 0x13);
    CHECK_EQ(b.soc.m.read(0x0001), 0xA5 & ~0x13);
}
