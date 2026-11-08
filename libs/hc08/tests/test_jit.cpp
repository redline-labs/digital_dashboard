// The JIT on hand-picked programs: flag-heavy arithmetic, the stack, indexed modes, code rewritten under a block, a
// device access that ends a block, and the cycle limit inside a block. Each runs on the interpreter and the JIT and
// must end identical. (Random programs: hc08_engine_diff --engine-diff --engine jit.)
#include <memory>

#include "hc08/jit/jit_engine.h"
#include "mctest/test.h"
#include "hc08/cpu.h"
#include "hc08/memmap.h"

using namespace sim;

namespace {

struct Env {
    std::array<uint8_t, 65536> mem{};
    std::array<uint8_t, 65536> region{};
    uint64_t limit = ~uint64_t(0);
    int device_reads = 0;
    DecodeCache* cache = nullptr;

    Env() {
        region.fill(uint8_t(Region::Ram));
        for (size_t a = 0; a < 0x40; a++) {
            region[a] = uint8_t(Region::Device);
        }
    }
    uint8_t fetch(uint16_t a) const {
        return mem[a];
    }
    uint8_t read(uint16_t a) {
        if (Region(region[a]) == Region::Device) {
            device_reads++;
        }
        return mem[a];
    }
    void write(uint16_t a, uint8_t v) {
        mem[a] = v;
        if (cache && cache->has_code(a)) {
            cache->invalidate(a);
        }
    }
    uint64_t cycle_limit() const {
        return limit;
    }
    bool irq_pending() const {
        return no_irq != 0;
    }
    uint16_t irq_vector() const {
        return 0xFFF2;
    }
    bool watching() const {
        return false;
    }
    bool pc_watched(uint16_t) const {
        return false;
    }
    bool irq_pin() const {
        return true;
    }
    uint8_t* jit_memory() {
        return mem.data();
    }
    const uint8_t* jit_regions() const {
        return region.data();
    }
    const uint64_t* jit_limit() const {
        return &limit;
    }
    const uint32_t* jit_irq_lines() const {
        return &no_irq;
    }
    uint32_t no_irq = 0;
};

struct Pair {
    Env a;
    Env b;
    CpuState sa;
    CpuState sb;
    DecodeCache cache_b;
    jit::JitCache jit{size_t(1) << 20};

    Pair() {
        b.cache = &cache_b;
        cache_b.on_drop = [this](unsigned page) { jit.drop_page(page); };
    }

    void load(uint16_t at, std::initializer_list<uint8_t> bytes) {
        for (uint8_t v : bytes) {
            a.mem[at] = v;
            b.mem[at] = v;
            at++;
        }
    }

    void start(uint16_t pc) {
        sa.pc = pc;
        sa.sp = 0x00FF;
        sa.ccr = ccr::kOnes | ccr::kI;
        sb = sa;
    }

    void run(uint64_t limit = ~uint64_t(0)) {
        a.limit = b.limit = limit;
        const Stop ra = interpret(sa, a);
        const Stop rb = jit::run_jit(sb, b, cache_b, jit);
        CHECK(ra.kind == rb.kind);
        CHECK_EQ(sb.a, sa.a);
        CHECK_EQ(sb.hx, sa.hx);
        CHECK_EQ(sb.sp, sa.sp);
        CHECK_EQ(sb.pc, sa.pc);
        CHECK_EQ(sb.ccr, sa.ccr);
        CHECK_EQ(sb.cycles, sa.cycles);
        CHECK(a.mem == b.mem);
        CHECK_EQ(b.device_reads, a.device_reads);
    }
};

}  // namespace

TEST(jit_arithmetic_flags_match) {
    if (!jit::host_supported()) {
        mctest::skip("no JIT assembler");
        return;
    }
    // every ALU op on a spread of values, results pushed so every intermediate is compared
    for (int value : {0x00, 0x01, 0x0F, 0x7F, 0x80, 0x99, 0xFF}) {
        Pair p;
        p.load(0x0400, {0xA6, uint8_t(value), 0xAB, 0x88, 0x85, 0x87,        // LDA ; ADD #$88 ; TPA ; PSHA
                        0xA9, 0x7F,           0x85, 0x87,                    // ADC #$7F ; TPA ; PSHA
                        0xA0, 0x90,           0x85, 0x87,                    // SUB #$90 ; TPA ; PSHA
                        0xA2, 0x01,           0x85, 0x87,                    // SBC #1 ; TPA ; PSHA
                        0x48, 0x49,           0x46, 0x47, 0x44, 0x40, 0x43,  // LSLA ROLA RORA ASRA LSRA NEGA COMA
                        0x85, 0x87,           0x4C, 0x4A, 0x4D, 0x85, 0x87,  // TPA PSHA INCA DECA TSTA TPA PSHA
                        0xAE, 0x13,           0x42, 0x87, 0x89, 0x8F});      // LDX #$13 ; MUL ; PSHA ; PSHX ; WAIT
        p.start(0x0400);
        p.run();
    }
}

TEST(jit_stack_calls_and_indexed_modes) {
    if (!jit::host_supported()) {
        mctest::skip("no JIT assembler");
        return;
    }
    Pair p;
    p.load(0x0400, {0x45, 0x05, 0x00,  // LDHX #$0500
                    0x6E, 0x07, 0x60,  // MOV #7,$60
                    0xCD, 0x05, 0x20,  // JSR $0520
                    0x35, 0x70,        // STHX $70
                    0x75, 0x70,        // CPHX $70
                    0x8F});            // WAIT
    p.load(0x0520,
           {0xE6, 0x03,        // LDA 3,X
            0x9E, 0xE7, 0x01,  // STA 1,SP (over the return address low byte: restored below)
            0x9E, 0x6F, 0x01,  // CLR 1,SP
            0xAF, 0x10,        // AIX #16
            0x3B, 0x60, 0xF5,  // DBNZ $60,-11 (back to LDA)
            0xA6, 0x0C, 0x9E, 0xE7, 0x02, 0x4F, 0x9E, 0xE7, 0x01,  // fix the return address to $040C... ($04 $0C)
            0x81});                                                // RTS
    for (size_t k = 0; k < 0x100; k++) {
        p.a.mem[0x0500 + k] = p.b.mem[0x0500 + k] = uint8_t(k * 7);
    }
    p.start(0x0400);
    p.run();
}

TEST(jit_code_rewritten_under_a_block) {
    if (!jit::host_supported()) {
        mctest::skip("no JIT assembler");
        return;
    }
    Pair p;
    // LDA #1 ; INCA ; STA $0401 (rewrites its own immediate) ; CMP #9 ; BNE back ; WAIT
    p.load(0x0400, {0xA6, 0x01, 0x4C, 0xC7, 0x04, 0x01, 0xA1, 0x09, 0x26, 0xF6, 0x8F});
    p.start(0x0400);
    p.run();
    CHECK_EQ(p.sb.a, 9);
    CHECK(p.jit.stats.dropped > 0);
}

TEST(jit_device_reads_and_the_limit_inside_a_block) {
    if (!jit::host_supported()) {
        mctest::skip("no JIT assembler");
        return;
    }
    for (const uint64_t limit : {1u, 3u, 5u, 8u, 13u, 21u, 34u, 55u}) {
        Pair p;
        // a loop reading a device register: LDA $20 ; INCA ; STA $80 ; BRA back
        p.load(0x0400, {0xB6, 0x20, 0x4C, 0xB7, 0x80, 0x20, 0xF9});
        p.start(0x0400);
        p.run(limit);
        p.run(limit * 3);
    }
}

TEST(jit_cli_delay_with_a_pending_interrupt) {
    if (!jit::host_supported()) {
        mctest::skip("no JIT assembler");
        return;
    }
    // a line asserted while interrupts are masked: SEI ; INCA ; CLI ; INCA (runs: CLI's delay) ; then the interrupt
    // is taken before the next instruction; the handler at $0480 counts in $90 and WAITs. Both engines must agree on
    // where it was taken and that it ran once.
    for (int variant = 0; variant < 3; variant++) {
        Pair p;
        p.load(0x0400, {0x9B, 0x4C, 0x9A, 0x4C, 0x4C, 0x4C, 0x9B, 0x8F});
        if (variant == 1) {
            p.load(0x0402, {0x9D, 0x9A});  // NOP ; CLI: the CLI one instruction later
        }
        if (variant == 2) {
            p.load(0x0403, {0x81});  // CLI ; RTS: the delayed instruction ends the block (its dispatch checks)
            p.a.mem[0x00FF] = p.b.mem[0x00FF] = 0x05;
            p.a.mem[0x00FE] = p.b.mem[0x00FE] = 0x04;
        }
        p.load(0x0480, {0x3C, 0x90, 0x8F});  // INC $90 ; WAIT
        p.a.mem[0xFFF2] = p.b.mem[0xFFF2] = 0x04;
        p.a.mem[0xFFF3] = p.b.mem[0xFFF3] = 0x80;
        p.a.no_irq = p.b.no_irq = 1;
        p.start(0x0400);
        if (variant == 2) {
            p.sa.sp = p.sb.sp = 0x00FD;
        }
        p.run();
        CHECK_EQ(p.sb.pc, p.sa.pc);
        CHECK_EQ(p.b.mem[0x90], 1);  // the handler ran once, after exactly one delayed instruction
    }
}
