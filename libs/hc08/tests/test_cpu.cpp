// The CPU08 core against the CPU08 Reference Manual: semantics, flags, cycles, the stack frame of interrupts, and the
// opcodes the HC08 does not have.
#include "flat_env.h"
#include "mctest/test.h"
#include "hc08/cpu.h"
#include "hc08/isa.h"

using namespace sim;
using sim::test::FlatEnv;
using sim::test::step;

namespace {

CpuState at(uint16_t pc) {
    CpuState s;
    s.pc = pc;
    s.ccr = ccr::kOnes;  // interrupts enabled, flags clear
    return s;
}

}  // namespace

TEST(cpu_add_sets_half_carry_overflow_and_carry) {
    FlatEnv env;
    env.load(0x8000, {0xAB, 0x88});  // ADD #$88
    CpuState s = at(0x8000);
    s.a = 0x88;
    step(s, env);
    CHECK_EQ(s.a, 0x10);
    CHECK(s.flag(ccr::kC));
    CHECK(s.flag(ccr::kV));
    CHECK(s.flag(ccr::kH));
    CHECK(!s.flag(ccr::kN));
    CHECK(!s.flag(ccr::kZ));
    CHECK_EQ(s.cycles, 2);
    CHECK_EQ(s.pc, 0x8002);
}

TEST(cpu_sbc_uses_borrow_and_sets_c_on_borrow) {
    FlatEnv env;
    env.load(0x8000, {0xA2, 0x01});  // SBC #1
    CpuState s = at(0x8000);
    s.a = 0x00;
    s.set_flag(ccr::kC, true);
    step(s, env);
    CHECK_EQ(s.a, 0xFE);
    CHECK(s.flag(ccr::kC));
    CHECK(s.flag(ccr::kN));
}

TEST(cpu_cmp_overflow_signed) {
    FlatEnv env;
    env.load(0x8000, {0xA1, 0x01});  // CMP #1 with A=$80: $80-1 = $7F, signed overflow
    CpuState s = at(0x8000);
    s.a = 0x80;
    step(s, env);
    CHECK(s.flag(ccr::kV));
    CHECK(!s.flag(ccr::kC));
    CHECK(!s.flag(ccr::kN));
}

TEST(cpu_neg_com_and_shifts) {
    FlatEnv env;
    CpuState s = at(0x8000);
    env.load(0x8000, {0x40});  // NEGA
    s.a = 0x80;
    step(s, env);
    CHECK_EQ(s.a, 0x80);
    CHECK(s.flag(ccr::kV));
    CHECK(s.flag(ccr::kC));

    env.load(0x8001, {0x43});  // COMA
    s.a = 0x0F;
    step(s, env);
    CHECK_EQ(s.a, 0xF0);
    CHECK(s.flag(ccr::kC));
    CHECK(!s.flag(ccr::kV));

    env.load(0x8002, {0x47});  // ASRA
    s.a = 0x81;
    step(s, env);
    CHECK_EQ(s.a, 0xC0);
    CHECK(s.flag(ccr::kC));
    CHECK(!s.flag(ccr::kV));  // N=1 C=1

    env.load(0x8003, {0x49});  // ROLA with C=1
    s.a = 0x40;
    step(s, env);
    CHECK_EQ(s.a, 0x81);
    CHECK(!s.flag(ccr::kC));
    CHECK(s.flag(ccr::kV));  // N=1 C=0

    env.load(0x8004, {0x44});  // LSRA
    s.a = 0x01;
    step(s, env);
    CHECK_EQ(s.a, 0);
    CHECK(s.flag(ccr::kZ));
    CHECK(s.flag(ccr::kC));
    CHECK(s.flag(ccr::kV));
}

TEST(cpu_inc_dec_overflow) {
    FlatEnv env;
    CpuState s = at(0x8000);
    env.load(0x8000, {0x4C, 0x4A});  // INCA, DECA
    s.a = 0x7F;
    step(s, env);
    CHECK(s.flag(ccr::kV));
    CHECK_EQ(s.a, 0x80);
    step(s, env);
    CHECK(s.flag(ccr::kV));
    CHECK_EQ(s.a, 0x7F);
}

TEST(cpu_mul_and_div) {
    FlatEnv env;
    CpuState s = at(0x8000);
    env.load(0x8000, {0x42});  // MUL
    s.a = 200;
    s.set_x(100);
    step(s, env);
    CHECK_EQ(s.x(), (20000 >> 8));
    CHECK_EQ(s.a, (20000 & 0xFF));
    CHECK(!s.flag(ccr::kC));
    CHECK_EQ(s.cycles, 5);

    env.load(0x8001, {0x52});  // DIV: H:A / X
    s.set_h(0x01);
    s.a = 0x00;
    s.set_x(0x10);
    step(s, env);
    CHECK_EQ(s.a, 0x10);
    CHECK_EQ(s.h(), 0x00);
    CHECK(!s.flag(ccr::kC));

    env.load(0x8002, {0x52});  // quotient overflow
    s.set_h(0x20);
    s.a = 0;
    s.set_x(0x10);
    step(s, env);
    CHECK(s.flag(ccr::kC));
}

TEST(cpu_daa_after_bcd_add) {
    FlatEnv env;
    CpuState s = at(0x8000);
    env.load(0x8000, {0xAB, 0x19, 0x72});  // ADD #$19 ; DAA  (A = $28)
    s.a = 0x28;
    step(s, env);
    step(s, env);
    CHECK_EQ(s.a, 0x47);
    CHECK(!s.flag(ccr::kC));
}

TEST(cpu_ldhx_sthx_cphx) {
    FlatEnv env;
    CpuState s = at(0x8000);
    env.load(0x8000, {0x45, 0x01, 0x14,    // LDHX #$0114
                      0x35, 0x40,          // STHX $40
                      0x55, 0x40,          // LDHX $40
                      0x65, 0x01, 0x15});  // CPHX #$0115
    step(s, env);
    CHECK_EQ(s.hx, 0x0114);
    CHECK_EQ(s.cycles, 3);
    step(s, env);
    CHECK_EQ(env.mem[0x40], 0x01);
    CHECK_EQ(env.mem[0x41], 0x14);
    step(s, env);
    CHECK_EQ(s.hx, 0x0114);
    step(s, env);
    CHECK(s.flag(ccr::kC));
    CHECK(s.flag(ccr::kN));
    CHECK(!s.flag(ccr::kZ));
}

TEST(cpu_indexed_modes_and_sp_relative) {
    FlatEnv env;
    CpuState s = at(0x8000);
    s.hx = 0x0200;
    s.sp = 0x0100;
    env.mem[0x0200] = 0x11;
    env.mem[0x0205] = 0x22;
    env.mem[0x0300] = 0x33;
    env.mem[0x0103] = 0x44;
    env.mem[0x0210] = 0x55;
    env.load(0x8000, {0xF6,                      // LDA ,X
                      0xE6, 0x05,                // LDA 5,X
                      0xD6, 0x01, 0x00,          // LDA $0100,X
                      0x9E, 0xE6, 0x03,          // LDA 3,SP
                      0x9E, 0xD6, 0x01, 0x10});  // LDA $0110,SP
    step(s, env);
    CHECK_EQ(s.a, 0x11);
    step(s, env);
    CHECK_EQ(s.a, 0x22);
    step(s, env);
    CHECK_EQ(s.a, 0x33);
    step(s, env);
    CHECK_EQ(s.a, 0x44);
    CHECK_EQ(s.cycles, 2 + 3 + 4 + 4);
    step(s, env);
    CHECK_EQ(s.a, 0x55);
    CHECK_EQ(s.cycles, 2 + 3 + 4 + 4 + 5);
}

TEST(cpu_jsr_rts_stack_order) {
    FlatEnv env;
    CpuState s = at(0x8000);
    s.sp = 0x00FF;
    env.load(0x8000, {0xCD, 0x90, 0x00});  // JSR $9000
    env.load(0x9000, {0x81});              // RTS
    step(s, env);
    CHECK_EQ(s.pc, 0x9000);
    CHECK_EQ(s.sp, 0x00FD);
    CHECK_EQ(env.mem[0x00FF], 0x03);  // PCL pushed first
    CHECK_EQ(env.mem[0x00FE], 0x80);
    CHECK_EQ(s.cycles, 5);
    step(s, env);
    CHECK_EQ(s.pc, 0x8003);
    CHECK_EQ(s.sp, 0x00FF);
}

TEST(cpu_jsr_indexed_calls_compiled_logic_style) {
    FlatEnv env;
    CpuState s = at(0x8000);
    s.hx = 0x0010;
    env.load(0x8000, {0xDD, 0x22, 0xBF});  // JSR opr16,X
    step(s, env);
    CHECK_EQ(s.pc, 0x22CF);
    CHECK_EQ(s.cycles, 6);
}

TEST(cpu_interrupt_frame_and_rti) {
    FlatEnv env;
    CpuState s = at(0x8000);
    s.sp = 0x00FF;
    s.a = 0xAA;
    s.hx = 0x12BB;
    s.ccr = ccr::kOnes | ccr::kC;
    env.mem[0xFFF2] = 0x83;
    env.mem[0xFFF3] = 0xB6;
    enter_interrupt(s, env, 0xFFF2, s.pc);
    CHECK_EQ(s.pc, 0x83B6);
    CHECK(s.flag(ccr::kI));
    CHECK_EQ(s.sp, 0x00FA);
    CHECK_EQ(env.mem[0x00FF], 0x00);  // PCL
    CHECK_EQ(env.mem[0x00FE], 0x80);  // PCH
    CHECK_EQ(env.mem[0x00FD], 0xBB);  // X (H is not stacked)
    CHECK_EQ(env.mem[0x00FC], 0xAA);  // A
    CHECK_EQ(env.mem[0x00FB], ccr::kOnes | ccr::kC);
    CHECK_EQ(s.cycles, 9);

    env.load(0x83B6, {0x80});  // RTI
    s.a = 0;
    s.set_x(0);
    step(s, env);
    CHECK_EQ(s.pc, 0x8000);
    CHECK_EQ(s.a, 0xAA);
    CHECK_EQ(s.x(), 0xBB);
    CHECK(!s.flag(ccr::kI));
    CHECK(s.flag(ccr::kC));
    CHECK_EQ(s.sp, 0x00FF);
}

TEST(cpu_run_core_takes_interrupt_after_cli_delay) {
    FlatEnv env;
    CpuState s = at(0x8000);
    s.ccr = ccr::kOnes | ccr::kI;
    s.sp = 0x00FF;
    env.load(0x8000, {0x9A, 0x9D, 0x9D});  // CLI, NOP, NOP
    env.mem[0xFFDE] = 0x90;
    env.mem[0xFFDF] = 0x00;
    env.irq_source_vector = 0xFFDE;
    env.limit = 2 + 1;  // CLI (2) + one NOP: the interrupt is taken before the second NOP
    run_core(s, env, [&env](uint16_t pc) { return decode(pc, [&env](uint16_t a) { return env.fetch(a); }); });
    CHECK_EQ(s.pc, 0x8002);  // CLI then one more instruction ran
    env.limit = 100;
    env.load(0x9000, {0x8F});  // WAIT
    const Stop stop =
        run_core(s, env, [&env](uint16_t pc) { return decode(pc, [&env](uint16_t a) { return env.fetch(a); }); });
    CHECK(stop.kind == StopKind::Wait);
    CHECK_EQ(env.mem[0x00FF], 0x02);  // return address $8002
}

TEST(cpu_cbeq_and_dbnz_forms) {
    FlatEnv env;
    CpuState s = at(0x8000);
    s.a = 5;
    s.hx = 0x0200;
    env.mem[0x0200] = 5;
    env.mem[0x40] = 2;
    env.load(0x8000, {
                         0x71,
                         0x10,  // CBEQ ,X+,+$10 (equal: branch, X incremented)
                     });
    step(s, env);
    CHECK_EQ(s.pc, 0x8012);
    CHECK_EQ(s.hx, 0x0201);
    CHECK_EQ(s.cycles, 4);

    env.load(0x8012, {0x3B, 0x40, 0xFD});  // DBNZ $40,-3
    step(s, env);
    CHECK_EQ(env.mem[0x40], 1);
    CHECK_EQ(s.pc, 0x8012);
    step(s, env);
    CHECK_EQ(env.mem[0x40], 0);
    CHECK_EQ(s.pc, 0x8015);

    env.load(0x8015, {0x41, 0x07, 0x02});  // CBEQA #7: not equal, falls through
    step(s, env);
    CHECK_EQ(s.pc, 0x8018);
}

TEST(cpu_mov_forms) {
    FlatEnv env;
    CpuState s = at(0x8000);
    s.hx = 0x0300;
    env.mem[0x0300] = 0x9A;
    env.mem[0x50] = 0x01;
    env.load(0x8000, {0x6E, 0x00, 0x60,  // MOV #0,$60
                      0x4E, 0x50, 0x61,  // MOV $50,$61
                      0x7E, 0x62,        // MOV ,X+,$62
                      0x5E, 0x50});      // MOV $50,X+
    step(s, env);
    CHECK_EQ(env.mem[0x60], 0);
    CHECK(s.flag(ccr::kZ));
    step(s, env);
    CHECK_EQ(env.mem[0x61], 1);
    step(s, env);
    CHECK_EQ(env.mem[0x62], 0x9A);
    CHECK(s.flag(ccr::kN));
    CHECK_EQ(s.hx, 0x0301);
    step(s, env);
    CHECK_EQ(env.mem[0x0301], 0x01);
    CHECK_EQ(s.hx, 0x0302);
    CHECK_EQ(s.cycles, 4 + 5 + 4 + 4);
}

TEST(cpu_brset_sets_carry_to_the_bit) {
    FlatEnv env;
    CpuState s = at(0x8000);
    env.mem[0x37] = 0x40;
    env.load(0x8000, {0x0D, 0x37, 0xFD});  // BRCLR 6,$37,* (falls through: bit set)
    step(s, env);
    CHECK_EQ(s.pc, 0x8003);
    CHECK(s.flag(ccr::kC));
    CHECK_EQ(s.cycles, 5);
}

TEST(cpu_signed_branches) {
    FlatEnv env;
    CpuState s = at(0x8000);
    env.load(0x8000, {0x92, 0x10});  // BGT
    s.set_flag(ccr::kN, true);
    s.set_flag(ccr::kV, true);
    step(s, env);
    CHECK_EQ(s.pc, 0x8012);
    s.pc = 0x8000;
    s.set_flag(ccr::kZ, true);
    step(s, env);
    CHECK_EQ(s.pc, 0x8002);
}

TEST(cpu_stack_pointer_transfers) {
    FlatEnv env;
    CpuState s = at(0x8000);
    s.sp = 0x0934;
    env.load(0x8000, {0x95, 0xA7, 0xFC, 0x94, 0x9C});  // TSX ; AIS #-4 ; TXS ; RSP
    step(s, env);
    CHECK_EQ(s.hx, 0x0935);
    step(s, env);
    CHECK_EQ(s.sp, 0x0930);
    step(s, env);
    CHECK_EQ(s.sp, 0x0934);
    step(s, env);
    CHECK_EQ(s.sp, 0x09FF);
}

TEST(cpu_hcs08_only_opcodes_are_illegal) {
    for (const uint8_t op : {uint8_t{0x32}, uint8_t{0x3E}, uint8_t{0x82}, uint8_t{0x8D}, uint8_t{0x96}, uint8_t{0xAC}}) {
        CHECK(kOpcodes[op].op == Op::Illegal);
    }
    CHECK(kOpcodes9E[0xAE].op == Op::Illegal);  // HCS08 LDHX ,X
    CHECK(kOpcodes9E[0xDC].op == Op::Illegal);  // no JMP/JSR SP forms
    FlatEnv env;
    CpuState s = at(0x8000);
    env.load(0x8000, {0x82});
    const Stop stop = step(s, env);
    CHECK(stop.kind == StopKind::Illegal);
    CHECK_EQ(s.pc, 0x8000);
}

TEST(cpu_every_legal_opcode_has_a_plausible_entry) {
    int legal = 0;
    for (size_t op = 0; op < 256; op++) {
        const OpInfo& info = kOpcodes[op];
        if (info.op == Op::Illegal) {
            continue;
        }
        legal++;
        mctest::check(info.length >= 1 && info.length <= 3, "length of opcode " + std::to_string(op));
        mctest::check(info.cycles >= 1 && info.cycles <= 9, "cycles of opcode " + std::to_string(op));
    }
    // 256 - $32 $3E $82 $8D $96 $9E(prefix) $AC = 249
    CHECK_EQ(legal, 249);
}
