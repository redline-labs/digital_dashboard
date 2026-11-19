// The e200 machine: the bus (RAM, ROM and its controller, device windows), devices on the schedule, the interrupt
// inputs and the wait state, the software TLB in front of the MMU, and code that rewrites itself -- on every engine.
#include <memory>

#include "mctest/test.h"
#include "e200/assembler.h"
#include "e200/machine.h"

using namespace sim::e200;
using sim::sys::Timing;

namespace {

constexpr uint32_t kRom = 0x00000000;
constexpr uint32_t kRam = 0x40000000;
constexpr uint32_t kDev = 0xC3F00000;

// A timer device: counts core cycles; at `period` it raises the machine's external input until its ACK register is
// written. Register 0: the count of expiries; register 4: write to acknowledge; register 8: the period.
class TickDevice final : public BusDevice {
   public:
    TickDevice() : BusDevice("tick", Timing::Timed) {
    }
    uint64_t period = 0;
    uint64_t next = sim::kNever;
    uint32_t expiries = 0;
    uint32_t last_write = 0;

    uint64_t next_event() const override {
        return next;
    }
    void sync(uint64_t now) override {
        while (next != sim::kNever && now >= next) {
            expiries++;
            m->set_external_input(true);
            next += period;
        }
    }
    uint32_t read(uint32_t offset, unsigned) override {
        return offset == 0 ? expiries : uint32_t(period);
    }
    void write(uint32_t offset, unsigned, uint32_t value) override {
        last_write = value;
        if (offset == 4) {
            m->set_external_input(false);
        } else if (offset == 8) {
            period = value;
            next = m->now() + period;
        }
    }
};

// A flat machine: a 1 MB supervisor/user page over ROM, one over RAM, one over the device, each identity-mapped.
std::unique_ptr<Machine> flat_machine(Engine engine) {
    MachineConfig cfg;
    cfg.engine = engine;
    auto m = std::make_unique<Machine>(cfg);
    m->map_rom(kRom, 0x100000);
    m->map_ram(kRam, 0x100000);
    m->state.tlb1[1] = TlbEntry{0x80000500, kRom, kRom | 0x3F};
    m->state.tlb1[2] = TlbEntry{0x80000500, kRam, kRam | 0x3F};
    m->state.tlb1[3] = TlbEntry{0x80000500, kDev | 0x08, kDev | 0x3F};  // cache-inhibited
    m->state.pc = kRom;
    m->translation_changed();
    return m;
}

void load(Machine& m, const Asm& a) {
    const auto bytes = a.bytes();
    CHECK(m.load_image(a.origin(), bytes.data(), bytes.size()));
}

const Engine kEngines[] = {Engine::Interpreter, Engine::Block, Engine::Jit};

}  // namespace

TEST(e200_machine_runs_from_rom_and_writes_ram) {
    for (Engine engine : kEngines) {
        auto m = flat_machine(engine);
        Asm a(kRom);
        auto spin = a.label();
        a.li32(3, kRam);
        a.li(4, 0);
        a.li(5, 100);
        a.mtctr(5);
        auto loop = a.label();
        a.bind(loop);
        a.addi(4, 4, 7);
        a.stwu(4, 4, 3);
        a.bdnz(loop);
        a.bind(spin);
        a.b(spin);
        load(*m, a);
        m->run_until(2000);
        CHECK_EQ(m->now(), 2000u);
        uint32_t v = 0;
        CHECK(m->peek(kRam + 400, 4, &v));
        CHECK_EQ(v, 700u);
        CHECK_EQ(m->state.gpr[3], kRam + 400);
    }
}

TEST(e200_machine_rom_writes_fault_without_a_controller) {
    auto m = flat_machine(Engine::Block);
    m->state.msr = kMsrMe;
    m->state.ivor[size_t(Vector::MachineCheck)] = 0x100;
    Asm a(kRom);
    a.li(3, 0x200);
    a.stw(3, 0, 3);
    load(*m, a);
    m->run_until(2);  // li, stw (and the entry)
    CHECK_EQ(m->state.pc, 0x100u);
    CHECK_EQ(m->state.mcsrr0, 0x4u);
}

TEST(e200_machine_device_interrupts_wake_the_core_from_wait) {
    for (Engine engine : kEngines) {
        auto m = flat_machine(engine);
        auto tick = std::make_shared<TickDevice>();
        m->add(tick);
        m->map(kDev, 0x1000, *tick);

        // the handler acknowledges and counts in r20, then returns into the wait loop
        Asm h(0x400);
        h.li32(10, kDev);
        h.stw(0, 4, 10);
        h.addi(20, 20, 1);
        h.rfi();
        load(*m, h);
        m->state.ivor[size_t(Vector::ExternalInput)] = 0x400;

        Asm a(kRom);
        auto idle = a.label();
        a.li32(10, kDev);
        a.li(3, 1000);
        a.stw(3, 8, 10);  // period: 1000 cycles
        a.wrteei(1);
        a.bind(idle);
        a.wait();
        a.b(idle);
        load(*m, a);

        m->run_until(10'500);
        CHECK_EQ(tick->expiries, 10u);
        CHECK_EQ(m->state.gpr[20], 10u);
        CHECK(m->waiting());
        // asleep most of the time: far fewer instructions than cycles (6 to start, 6 per tick)
        CHECK(m->stats().interrupts == 10);
    }
}

TEST(e200_machine_device_access_mid_quantum_sees_the_cycle) {
    auto m = flat_machine(Engine::Block);
    auto tick = std::make_shared<TickDevice>();
    m->add(tick);
    m->map(kDev, 0x1000, *tick);
    Asm a(kRom);
    auto spin = a.label();
    a.li32(10, kDev);
    a.li(3, 50);
    a.stw(3, 8, 10);  // at cycle 4: next expiry at 54
    for (int i = 0; i < 60; ++i) {
        a.nop();
    }
    a.lwz(4, 0, 10);  // at cycle 65: one expiry
    a.bind(spin);
    a.b(spin);
    load(*m, a);
    m->run_until(100);
    CHECK_EQ(m->state.gpr[4], 1u);
}

TEST(e200_machine_code_written_in_ram_is_decoded_again) {
    for (Engine engine : kEngines) {
        auto m = flat_machine(engine);
        // RAM holds `li r3,1; blr`; ROM calls it, rewrites it to `li r3,2`, and calls it again
        Asm f(kRam);
        f.li(3, 1);
        f.blr();
        load(*m, f);
        Asm patch(0);
        patch.li(3, 2);
        const uint32_t new_word = patch.words()[0];

        Asm a(kRom);
        auto spin = a.label();
        a.li32(9, kRam);
        a.mtctr(9);
        a.bctrl();
        a.mr(20, 3);
        a.li32(5, new_word);
        a.stw(5, 0, 9);
        a.mtctr(9);
        a.bctrl();
        a.mr(21, 3);
        a.bind(spin);
        a.b(spin);
        load(*m, a);
        m->run_until(200);
        CHECK_EQ(m->state.gpr[20], 1u);
        CHECK_EQ(m->state.gpr[21], 2u);
        if (engine == Engine::Block) {
            CHECK(m->stats().invalidated >= 1);
        }
    }
}

TEST(e200_machine_soft_tlb_follows_tlbwe_and_pid) {
    auto m = flat_machine(Engine::Block);
    // EA 0x10000000 maps to RAM page 0 for PID 5 only, then to RAM page 1
    uint32_t v = 0x11111111;
    CHECK(m->load_image(kRam, reinterpret_cast<const uint8_t*>("\x11\x11\x11\x11"), 4));
    CHECK(m->load_image(kRam + 0x1000, reinterpret_cast<const uint8_t*>("\x22\x22\x22\x22"), 4));
    m->state.tlb1[4] = TlbEntry{0x80050100, 0x10000000, kRam | 0x3F};
    m->state.pid0 = 5;
    m->translation_changed();
    CHECK(m->load(0x10000000, 4, &v) == Fault::None);
    CHECK_EQ(v, 0x11111111u);

    m->state.tlb1[4].mas3 = (kRam + 0x1000) | 0x3F;
    m->translation_changed();
    CHECK(m->load(0x10000000, 4, &v) == Fault::None);
    CHECK_EQ(v, 0x22222222u);

    m->state.pid0 = 6;
    m->translation_changed();
    CHECK(m->load(0x10000000, 4, &v) == Fault::TlbMiss);
}

TEST(e200_machine_access_across_a_page_boundary) {
    auto m = flat_machine(Engine::Block);
    CHECK(m->store(kRam + 0xFFE, 4, 0xA1B2C3D4) == Fault::None);
    uint32_t v = 0;
    CHECK(m->load(kRam + 0xFFE, 4, &v) == Fault::None);
    CHECK_EQ(v, 0xA1B2C3D4u);
    CHECK(m->load(kRam + 0x1000, 2, &v) == Fault::None);
    CHECK_EQ(v, 0xC3D4u);
}

TEST(e200_machine_pc_watch_and_vector_catch_stop_the_run) {
    auto m = flat_machine(Engine::Block);
    Asm a(kRom);
    a.nop();
    a.nop();
    a.sc();
    load(*m, a);
    m->state.ivor[size_t(Vector::SystemCall)] = 0x800;
    m->watch_pc(kRom + 4, true);
    Stop stop = m->run_until(100);
    CHECK(stop.kind == StopKind::Watch);
    CHECK_EQ(stop.pc, kRom + 4);
    m->watch_pc(kRom + 4, false);
    m->catch_vector(Vector::SystemCall, true);
    stop = m->run_until(100);
    CHECK(stop.kind == StopKind::Break);
    CHECK_EQ(m->state.pc, 0x800u);
}

TEST(e200_machine_decrementer_wakes_the_core_from_wait) {
    auto m = flat_machine(Engine::Block);
    Asm h(0x900);
    h.li32(3, kTsrDis);
    h.mtspr(spr::kTsr, 3);  // acknowledge
    h.addi(20, 20, 1);
    h.rfi();
    load(*m, h);
    m->state.ivor[size_t(Vector::Decrementer)] = 0x900;
    Asm a(kRom);
    auto idle = a.label();
    a.li(3, 500);
    a.mtspr(spr::kDecar, 3);
    a.mtspr(spr::kDec, 3);
    a.li32(3, kTcrDie | kTcrAre);
    a.mtspr(spr::kTcr, 3);
    a.li32(3, kHid0Tben);
    a.mtspr(spr::kHid0, 3);
    a.wrteei(1);
    a.bind(idle);
    a.wait();
    a.b(idle);
    load(*m, a);
    m->run_until(5'100);
    CHECK_EQ(m->state.gpr[20], 10u);
}
