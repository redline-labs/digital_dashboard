// The machine: device timing and interrupts, WAIT, cut runs equal uncut ones, snapshots, decode-cache invalidation by
// writes to code in RAM.
#include <memory>

#include "hc08/machine.h"
#include "mctest/test.h"
#include "emu/irq_line.h"

using namespace sim;
using namespace sim::sys;

namespace {

// A periodic interrupt every `period` ticks; register 0 reads the count and acknowledges.
class Ticker final : public Periph {
   public:
    explicit Ticker(uint64_t period) : Periph("ticker", Timing::Timed), period_(period), next_(period) {
    }

    IrqLine line;
    uint32_t count = 0;

    uint64_t next_event() const override {
        return next_;
    }

    void sync(uint64_t now) override {
        while (now >= next_) {
            next_ += period_;
            count++;
            line.set(true, now);
        }
    }

    void save(std::vector<uint8_t>& out) const override {
        out.push_back(uint8_t(count));
        for (int k = 0; k < 8; k++) {
            out.push_back(uint8_t(next_ >> (8 * k)));
        }
    }

    void load(const uint8_t*& in) override {
        count = *in++;
        next_ = 0;
        for (int k = 0; k < 8; k++) {
            next_ |= uint64_t(*in++) << (8 * k);
        }
    }

   protected:
    uint8_t rd(uint16_t) override {
        line.set(false, 0);
        return uint8_t(count);
    }
    void wr(uint16_t, uint8_t) override {
    }

   private:
    uint64_t period_;
    uint64_t next_;
};

// RAM $0040-$043F, flash $8000-$FFFF; the ticker at $0010 on vector $FFF2; main: CLI; loop: WAIT; BRA loop. The
// handler increments $50 and reads the ticker (acknowledge), RTI.
std::unique_ptr<Machine> make_machine(CpuEngine engine, uint64_t period) {
    auto m = std::make_unique<Machine>(MachineConfig{.engine = engine});
    m->mem.add_ram(0x0040, 0x400);
    m->mem.add_flash(0x8000, 0x8000);
    auto ticker = std::make_shared<Ticker>(period);
    ticker->line = IrqLine(m.get(), irq::kTim1Overflow);
    m->add(ticker);
    m->map(0x0010, 1, *ticker);

    const uint8_t main_code[] = {0x9A, 0x8F, 0x20, 0xFD};      // CLI ; WAIT ; BRA *-1
    const uint8_t handler[] = {0x3C, 0x50, 0xB6, 0x10, 0x80};  // INC $50 ; LDA $10 ; RTI
    for (size_t k = 0; k < sizeof(main_code); k++) {
        m->mem.bytes[0x8000 + k] = main_code[k];
    }
    for (size_t k = 0; k < sizeof(handler); k++) {
        m->mem.bytes[0x9000 + k] = handler[k];
    }
    m->mem.bytes[0xFFF2] = 0x90;
    m->mem.bytes[0xFFF3] = 0x00;
    m->mem.bytes[0xFFFE] = 0x80;
    m->mem.bytes[0xFFFF] = 0x00;
    m->reset(ResetCause::PowerOn);
    return m;
}

}  // namespace

TEST(machine_periodic_interrupt_wakes_wait) {
    auto m = make_machine(CpuEngine::Interpreter, 32'000);  // 1 ms
    m->run_until(32'000 * 10 + 1000);
    CHECK_EQ(m->mem.bytes[0x50], 10);
    CHECK(m->mode() == CpuMode::Wait);
}

TEST(machine_cut_runs_equal_one_run) {
    auto whole = make_machine(CpuEngine::Block, 3'333);
    auto cut = make_machine(CpuEngine::Block, 3'333);
    whole->run_until(1'000'000);
    for (uint64_t t = 7'777; t < 1'000'000; t += 7'777) {
        cut->run_until(t);
    }
    cut->run_until(1'000'000);
    CHECK_EQ(cut->cpu.cycles, whole->cpu.cycles);
    CHECK_EQ(cut->mem.bytes[0x50], whole->mem.bytes[0x50]);
    CHECK_EQ(cut->now(), whole->now());
}

TEST(machine_engines_agree_on_the_ticker_program) {
    auto a = make_machine(CpuEngine::Interpreter, 5'000);
    auto b = make_machine(CpuEngine::Block, 5'000);
    a->run_until(2'000'000);
    b->run_until(2'000'000);
    CHECK_EQ(a->cpu.cycles, b->cpu.cycles);
    CHECK_EQ(a->cpu.pc, b->cpu.pc);
    CHECK_EQ(a->mem.bytes[0x50], b->mem.bytes[0x50]);
}

TEST(machine_snapshot_restore_replays_identically) {
    auto m = make_machine(CpuEngine::Block, 4'000);
    m->run_until(100'000);
    const Machine::Snapshot snap = m->snapshot();
    m->run_until(500'000);
    const uint64_t cycles = m->cpu.cycles;
    const uint8_t count = m->mem.bytes[0x50];
    m->restore(snap);
    m->run_until(500'000);
    CHECK_EQ(m->cpu.cycles, cycles);
    CHECK_EQ(m->mem.bytes[0x50], count);
}

TEST(machine_pc_watch_stops_before_the_instruction) {
    auto m = make_machine(CpuEngine::Block, 32'000);
    m->watch_pc(0x9000, true);
    const Stop stop = m->run_until(32'000 * 5);
    CHECK(stop.kind == StopKind::Watch);
    CHECK_EQ(stop.pc, 0x9000);
    CHECK_EQ(m->mem.bytes[0x50], 0);
    m->watch_pc(0x9000, false);
    m->run_until(32'000 * 5);
    CHECK_EQ(m->mem.bytes[0x50], 4);
}

TEST(machine_data_watch_breaks_after_the_write) {
    auto m = make_machine(CpuEngine::Block, 32'000);
    int hits = 0;
    m->add_data_watch(Machine::DataWatch{0x50, 0x51, false, true, [&hits](uint16_t, uint8_t value, bool) {
                                             hits++;
                                             return value == 3;
                                         }});
    const Stop stop = m->run_until(32'000 * 10);
    CHECK(stop.kind == StopKind::Break);
    CHECK_EQ(hits, 3);
    CHECK_EQ(m->mem.bytes[0x50], 3);
}

TEST(machine_write_to_ram_code_invalidates_decode) {
    Machine m(MachineConfig{.engine = CpuEngine::Block});
    m.mem.add_ram(0x0040, 0x400);
    m.mem.add_flash(0x8000, 0x8000);
    // RAM code at $0100: LDA #1 ; STA $0101 (patches its own immediate to 1+?) ; WAIT
    const uint8_t code[] = {0xA6, 0x01, 0x4C, 0xC7, 0x01, 0x01, 0x20, 0xF8};  // LDA #n ; INCA ; STA $0101 ; BRA -8
    for (size_t k = 0; k < sizeof(code); k++) {
        m.mem.bytes[0x0100 + k] = code[k];
    }
    m.mem.bytes[0xFFFE] = 0x01;
    m.mem.bytes[0xFFFF] = 0x00;
    m.reset(ResetCause::PowerOn);
    m.run_until(4 * (2 + 1 + 4 + 3) * 5);  // five passes
    CHECK_EQ(m.mem.bytes[0x0101], 6);
    CHECK(m.decode_cache().invalidations() >= 5);
}
