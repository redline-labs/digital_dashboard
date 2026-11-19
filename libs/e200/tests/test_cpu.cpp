// The e200 core against Book E and the e200z6 manual: integer semantics and the XER/CR they set, branches, loads and
// stores, the reservation, interrupts and their save/restore registers, privilege, the decrementer, the MMU, and the
// SPE's single-precision arithmetic. Programs are assembled here (e200/assembler.h); no target toolchain.
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <random>

#include "flat_env.h"
#include "mctest/test.h"
#include "e200/cpu.h"
#include "e200/isa.h"

using namespace sim::e200;
using sim::e200::test::core_at;
using sim::e200::test::FlatEnv;
using sim::e200::test::run_for;

namespace {

uint32_t f32(float f) {
    return std::bit_cast<uint32_t>(f);
}

// Runs `a` from its origin until it reaches `end` (a label bound at the end) or `max` instructions.
Stop run_to(CpuState& s, FlatEnv& env, const Asm& a, Asm::Label end, uint64_t max = 10000) {
    env.put(a);
    s.pc = a.origin();
    while (s.pc != a.address(end) && max-- > 0) {
        const Stop stop = run_for(s, env, 1);
        if (stop.kind != StopKind::Limit) {
            return stop;
        }
    }
    return Stop{StopKind::Limit, s.pc, 0};
}

}  // namespace

TEST(e200_add_carry_overflow_and_record) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto end = a.label();
    a.li32(3, 0x7FFFFFFF);
    a.li(4, 1);
    a.add(5, 3, 4, true, true);   // addo. : overflow, negative
    a.li32(6, 0xFFFFFFFF);
    a.addc(7, 6, 4);              // carry out, result 0
    a.adde(8, 4, 4);              // 1 + 1 + CA
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[5], 0x80000000u);
    CHECK_EQ(s.cr_field(0), 0x8u | 0x1u);  // LT, SO (copied from XER after OV)
    CHECK_EQ(s.gpr[7], 0u);
    CHECK_EQ(s.gpr[8], 3u);
    CHECK((s.xer & kXerSo) != 0);
    CHECK((s.xer & kXerCa) == 0);  // adde of 1+1+1 carries nothing out
}

TEST(e200_subtract_family_sets_carry_as_not_borrow) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto end = a.label();
    a.li(3, 5);
    a.li(4, 7);
    a.subfc(5, 3, 4);  // 7 - 5: no borrow, CA = 1
    a.subfc(6, 4, 3);  // 5 - 7: borrow, CA = 0
    a.subfic(7, 3, 2);  // 2 - 5
    a.neg(8, 4);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[5], 2u);
    CHECK_EQ(s.gpr[6], uint32_t(-2));
    CHECK_EQ(s.gpr[7], uint32_t(-3));
    CHECK((s.xer & kXerCa) == 0);
    CHECK_EQ(s.gpr[8], uint32_t(-7));
}

TEST(e200_multiply_divide_and_their_edges) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto end = a.label();
    a.li32(3, 0x80000000);
    a.li(4, -1);
    a.li(9, 0);
    a.mulhw(5, 3, 3);    // (-2^31)^2 >> 32 = 2^30
    a.mulhwu(6, 4, 4);   // (2^32-1)^2 >> 32
    a.divw(7, 3, 4);     // INT_MIN / -1: saturates
    a.divwu(8, 4, 9);    // / 0
    a.mulli(10, 4, 1000);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[5], 0x40000000u);
    CHECK_EQ(s.gpr[6], 0xFFFFFFFEu);
    CHECK_EQ(s.gpr[7], 0x7FFFFFFFu);
    CHECK_EQ(s.gpr[8], 0xFFFFFFFFu);
    CHECK_EQ(s.gpr[10], uint32_t(-1000));
}

TEST(e200_rotates_shifts_and_algebraic_shift_carry) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto end = a.label();
    a.li32(3, 0x12345678);
    a.rlwinm(4, 3, 8, 24, 31);      // the top byte
    a.rlwinm(5, 3, 0, 28, 3);       // wrapping mask: 0xF000000F
    a.li32(6, 0xFFFF0000);
    a.rlwimi(6, 3, 0, 16, 31);      // insert the low half
    a.li32(7, 0xFFFFFFF1);
    a.srawi(8, 7, 4);               // -1, CA = 1 (ones shifted out of a negative)
    a.li(9, 40);
    a.slw(10, 3, 9);                // shift by >31: 0
    a.cntlzw(11, 4);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[4], 0x12u);
    CHECK_EQ(s.gpr[5], 0x10000008u);
    CHECK_EQ(s.gpr[6], 0xFFFF5678u);
    CHECK_EQ(s.gpr[8], 0xFFFFFFFFu);
    CHECK((s.xer & kXerCa) != 0);
    CHECK_EQ(s.gpr[10], 0u);
    CHECK_EQ(s.gpr[11], 27u);
}

TEST(e200_loop_with_ctr_compares_and_isel) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto loop = a.label();
    auto end = a.label();
    a.li(3, 0);
    a.li(4, 10);
    a.mtctr(4);
    a.bind(loop);
    a.addi(3, 3, 3);
    a.bdnz(loop);
    a.cmpwi(7, 3, 30);
    a.li(5, 111);
    a.li(6, 222);
    a.isel(8, 5, 6, 7 * 4 + 2);     // cr7.eq ? r5 : r6
    a.cmplwi(1, 3, 31);
    a.isel(9, 5, 6, 1 * 4 + 1);     // cr1.gt ? r5 : r6
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[3], 30u);
    CHECK_EQ(s.ctr, 0u);
    CHECK_EQ(s.gpr[8], 111u);
    CHECK_EQ(s.gpr[9], 222u);
    CHECK_EQ(s.cr_field(1), 0x8u);
}

TEST(e200_call_and_return_through_lr) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto fn = a.label();
    auto end = a.label();
    auto over = a.label();
    a.li(3, 1);
    a.bl(fn);
    a.addi(3, 3, 100);
    a.b(over);
    a.bind(fn);
    a.addi(3, 3, 10);
    a.blr();
    a.bind(over);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[3], 111u);
    CHECK_EQ(s.lr, 0x1008u);
}

TEST(e200_loads_and_stores_are_big_endian_with_update_forms) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    env.put32(0x8000, 0x8899AABB);
    Asm a(0x1000);
    auto end = a.label();
    a.li32(3, 0x8000);
    a.lwz(4, 0, 3);
    a.lbz(5, 1, 3);
    a.lha(6, 0, 3);       // sign-extended 0x8899
    a.lhz(7, 2, 3);
    a.lwbrx(8, 0, 3);
    a.li32(9, 0x01020304);
    a.stwu(9, 16, 3);     // r3 becomes 0x8010
    a.sth(9, 4, 3);
    a.stb(9, 7, 3);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[4], 0x8899AABBu);
    CHECK_EQ(s.gpr[5], 0x99u);
    CHECK_EQ(s.gpr[6], 0xFFFF8899u);
    CHECK_EQ(s.gpr[7], 0xAABBu);
    CHECK_EQ(s.gpr[8], 0xBBAA9988u);
    CHECK_EQ(s.gpr[3], 0x8010u);
    CHECK_EQ(env.get32(0x8010), 0x01020304u);
    CHECK_EQ(env.get32(0x8014), 0x03040004u);
}

TEST(e200_load_and_store_multiple) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto end = a.label();
    a.li32(1, 0x9000);
    for (int r = 28; r < 32; ++r) {
        a.li(r, int16_t(r * 3));
    }
    a.stmw(28, 0, 1);
    for (int r = 28; r < 32; ++r) {
        a.li(r, 0);
    }
    a.lmw(29, 4, 1);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(env.get32(0x9000), 84u);
    CHECK_EQ(env.get32(0x900C), 93u);
    CHECK_EQ(s.gpr[28], 0u);
    CHECK_EQ(s.gpr[29], 87u);
    CHECK_EQ(s.gpr[31], 93u);
}

TEST(e200_reservation_store_conditional) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto end = a.label();
    a.li32(3, 0x9000);
    a.li(4, 5);
    a.stwcx_(4, 0, 3);    // no reservation: fails, CR0.EQ clear
    a.mfcr(10);
    a.lwarx(5, 0, 3);
    a.addi(5, 5, 1);
    a.stwcx_(5, 0, 3);    // succeeds
    a.mfcr(11);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[10] >> 28, 0u);
    CHECK_EQ(s.gpr[11] >> 28, 2u);
    CHECK_EQ(env.get32(0x9000), 1u);
}

TEST(e200_condition_register_moves) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto end = a.label();
    a.li32(3, 0x12345678);
    a.mtcrf(0x81, 3);      // fields 0 and 7
    a.crxor(1, 1, 1);      // clear cr0.gt
    a.cror(31, 3, 3);      // cr7.so = cr0.so
    a.mfcr(4);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[4], 0x10000009u);
}

TEST(e200_system_call_saves_state_and_rfi_returns) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    s.ivpr = 0x00040000;
    s.ivor[size_t(Vector::SystemCall)] = 0x0800;
    s.msr = kMsrEe | kMsrPr;  // user state, interrupts on
    Asm handler(0x40800);
    handler.li(3, 42);
    handler.rfi();
    env.put(handler);
    Asm a(0x1000);
    auto end = a.label();
    a.sc();
    a.addi(3, 3, 1);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[3], 43u);
    CHECK_EQ(s.srr0, 0x1004u);
    CHECK_EQ(s.srr1, kMsrEe | kMsrPr);
    CHECK_EQ(s.msr, kMsrEe | kMsrPr);  // restored
    CHECK(env.interrupts.size() == 1 && env.interrupts[0] == Vector::SystemCall);
}

TEST(e200_user_state_cannot_run_privileged_instructions) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    s.ivpr = 0x00040000;
    s.ivor[size_t(Vector::Program)] = 0x0600;
    s.msr = kMsrPr;
    Asm a(0x1000);
    a.mfmsr(3);
    env.put(a);
    run_for(s, env, 1);
    CHECK_EQ(s.pc, 0x40600u);
    CHECK_EQ(s.srr0, 0x1000u);  // the instruction, not past it
    CHECK_EQ(s.esr, kEsrPpr);
    CHECK(s.supervisor());
}

TEST(e200_user_mfspr_of_a_supervisor_spr_is_privileged_and_unknown_spr_illegal) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    s.ivpr = 0x00040000;
    s.ivor[size_t(Vector::Program)] = 0x0600;
    s.msr = kMsrPr;
    Asm a(0x1000);
    a.mfspr(3, spr::kSrr0);
    env.put(a);
    run_for(s, env, 1);
    CHECK_EQ(s.esr, kEsrPpr);

    CpuState t = core_at(0x2000);
    t.ivpr = 0x00040000;
    t.ivor[size_t(Vector::Program)] = 0x0600;
    Asm b(0x2000);
    b.mfspr(3, 1000);  // nothing there
    b.word(0x00000000);
    env.put(b);
    run_for(t, env, 1);
    CHECK_EQ(t.esr, kEsrPil);
    CHECK_EQ(t.pc, 0x40600u);
}

TEST(e200_trap_and_illegal_take_the_program_interrupt) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    s.ivor[size_t(Vector::Program)] = 0x0600;
    Asm a(0x1000);
    a.li(3, 4);
    a.twi(4, 3, 5);   // eq: not taken
    a.twi(0x10, 3, 5);  // lt: taken
    env.put(a);
    run_for(s, env, 3);
    CHECK_EQ(s.pc, 0x600u);
    CHECK_EQ(s.srr0, 0x1008u);
    CHECK_EQ(s.esr, kEsrPtr);
}

TEST(e200_external_input_is_taken_only_with_ee) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    s.ivor[size_t(Vector::ExternalInput)] = 0x0400;
    Asm a(0x1000);
    a.nop();
    a.wrteei(1);
    a.nop();
    env.put(a);
    env.external = true;
    run_for(s, env, 2);   // nop, wrteei: EE was clear before them
    CHECK_EQ(s.pc, 0x1008u);
    run_for(s, env, 1);   // taken before the next instruction
    CHECK_EQ(s.pc, 0x400u);
    CHECK_EQ(s.srr0, 0x1008u);
    CHECK((s.msr & kMsrEe) == 0);
}

TEST(e200_decrementer_counts_with_the_timebase_and_interrupts) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    s.ivor[size_t(Vector::Decrementer)] = 0x0900;
    Asm a(0x1000);
    auto loop = a.label();
    a.li(3, 20);
    a.mtspr(spr::kDec, 3);
    a.li32(3, kTcrDie);
    a.mtspr(spr::kTcr, 3);
    a.li32(3, kHid0Tben);
    a.mtspr(spr::kHid0, 3);
    a.wrteei(1);
    a.bind(loop);
    a.b(loop);
    env.put(a);
    Asm h(0x900);
    auto spin = h.label();
    h.bind(spin);
    h.b(spin);
    env.put(h);
    run_for(s, env, 10);
    CHECK_EQ(s.decrementer(), 18u);  // TBEN set by the 8th instruction; two more have run
    run_for(s, env, 30);
    CHECK_EQ(s.pc, 0x900u);
    CHECK((s.tsr & kTsrDis) != 0);
    CHECK(env.interrupts.size() >= 1 && env.interrupts[0] == Vector::Decrementer);
    CHECK_EQ(s.decrementer(), 0u);  // no auto-reload: it stopped at 0
}

TEST(e200_wait_stops_the_core_past_the_instruction) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    a.wait();
    env.put(a);
    const Stop stop = run_for(s, env, 5);
    CHECK(stop.kind == StopKind::Wait);
    CHECK_EQ(s.pc, 0x1004u);
}

TEST(e200_tlb_miss_then_tlbwe_maps_the_page) {
    FlatEnv env;
    env.mmu = true;
    CpuState s;
    reset_core(s);
    // a 1 MB supervisor page mapping EA 0x00000000 to PA 0 for the code, so translation is on from the start
    s.tlb1[1] = TlbEntry{0x80000500, 0x00000000, 0x0000003F};
    s.pc = 0x1000;
    s.ivor[size_t(Vector::DataTlbError)] = 0x0D00;
    s.mas[4] = 0x10000100;  // TLB1, 4 KB default
    Asm a(0x1000);
    a.li32(3, 0x40000010);
    a.lwz(4, 0, 3);       // EA 0x40000000: not mapped
    env.put(a);
    run_for(s, env, 3);
    CHECK_EQ(s.pc, 0xD00u);
    CHECK_EQ(s.dear, 0x40000010u);
    CHECK_EQ(s.srr0, 0x1008u);
    CHECK_EQ(s.mas[1] & 0x80000F00u, 0x80000100u);
    CHECK_EQ(s.mas[2] & 0xFFFFF000u, 0x40000000u);

    // the handler maps the page onto physical 0x8000 and returns
    env.put32(0x8010, 0xCAFEF00D);
    Asm h(0xD00);
    h.li32(5, 0x10050000);  // TLB1, entry 5
    h.mtspr(spr::kMas0, 5);
    h.li32(5, 0x80000100);
    h.mtspr(spr::kMas1, 5);
    h.li32(5, 0x40000000);
    h.mtspr(spr::kMas2, 5);
    h.li32(5, 0x0000803F);
    h.mtspr(spr::kMas3, 5);
    h.tlbwe();
    h.rfi();
    env.put(h);
    run_for(s, env, 15);  // the handler's 14 instructions, and the load again
    CHECK_EQ(s.gpr[4], 0xCAFEF00Du);
    CHECK_EQ(s.pc, 0x100Cu);
    CHECK(env.translation_changes >= 1);
}

TEST(e200_user_write_to_a_read_only_page_is_a_data_storage_interrupt) {
    FlatEnv env;
    env.mmu = true;
    CpuState s;
    reset_core(s);
    s.tlb1[1] = TlbEntry{0x80000500, 0x00000000, 0x00000033};  // SX UX SR UR: no writes
    s.ivor[size_t(Vector::DataStorage)] = 0x0200;
    s.msr = kMsrPr;
    s.pc = 0x1000;
    Asm a(0x1000);
    a.li(3, 0x100);
    a.stw(3, 0, 3);
    env.put(a);
    run_for(s, env, 2);
    CHECK_EQ(s.pc, 0x200u);
    CHECK_EQ(s.esr, kEsrSt);
    CHECK_EQ(s.dear, 0x100u);
}

TEST(e200_instruction_tlb_miss_and_tlbsx) {
    FlatEnv env;
    env.mmu = true;
    CpuState s;
    reset_core(s);
    s.tlb1[3] = TlbEntry{0x80000500, 0x00000000, 0x0000003F};
    s.ivor[size_t(Vector::InstructionTlbError)] = 0x0E00;
    s.pc = 0x00200000;  // outside the 1 MB page
    run_for(s, env, 1);
    CHECK_EQ(s.pc, 0xE00u);
    CHECK_EQ(s.srr0, 0x00200000u);

    tlb_search(s, 0x00080000);
    CHECK_EQ((s.mas[0] >> 16) & 0x1Fu, 3u);
    CHECK_EQ(s.mas[1], 0x80000500u);
}

TEST(e200_reset_maps_the_boot_page_and_starts_at_the_reset_vector) {
    CpuState s;
    reset_core(s);
    CHECK_EQ(s.pc, 0xFFFFFFFCu);
    uint32_t pa = 0;
    CHECK(mmu_translate(s, 0xFFFFFFFC, Access::Execute, &pa) == Fault::None);
    CHECK_EQ(pa, 0xFFFFFFFCu);
    CHECK(mmu_translate(s, 0xFFFFE000, Access::Execute, &pa) == Fault::TlbMiss);
}

TEST(e200_bus_error_is_a_machine_check_or_a_checkstop) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    a.li32(3, 0x40000000);
    a.lwz(4, 0, 3);
    env.put(a);
    const Stop stop = run_for(s, env, 3);
    CHECK(stop.kind == StopKind::Checkstop);  // MSR[ME] clear

    CpuState t = core_at(0x1000);
    t.msr = kMsrMe;
    t.ivor[size_t(Vector::MachineCheck)] = 0x0100;
    run_for(t, env, 3);
    CHECK_EQ(t.pc, 0x100u);
    CHECK_EQ(t.mcsrr0, 0x1008u);
}

TEST(e200_spe_single_precision_arithmetic_and_conversions) {
    FlatEnv env;
    CpuState s = core_at(0x1000);
    Asm a(0x1000);
    auto end = a.label();
    a.li32(3, f32(1.5f));
    a.li32(4, f32(2.25f));
    a.efsadd(5, 3, 4);
    a.efsmul(6, 3, 4);
    a.efsdiv(7, 4, 3);
    a.efssub(8, 3, 4);
    a.li(9, -7);
    a.efscfsi(10, 9);
    a.efsctsi(11, 6);      // 3.375 -> 3 (nearest)
    a.li32(12, f32(2.5f));
    a.efsctsi(13, 12);     // ties to even: 2
    a.li32(14, f32(-2.7f));
    a.efsctsiz(15, 14);    // toward zero: -2
    a.efscmpgt(2, 4, 3);
    a.efscmplt(3, 4, 3);
    a.bind(end);
    run_to(s, env, a, end);
    CHECK_EQ(s.gpr[5], f32(3.75f));
    CHECK_EQ(s.gpr[6], f32(3.375f));
    CHECK_EQ(s.gpr[7], f32(1.5f));
    CHECK_EQ(s.gpr[8], f32(-0.75f));
    CHECK_EQ(s.gpr[10], f32(-7.0f));
    CHECK_EQ(s.gpr[11], 3u);
    CHECK_EQ(s.gpr[13], 2u);
    CHECK_EQ(s.gpr[15], uint32_t(-2));
    CHECK_EQ(s.cr_field(2), 4u);
    CHECK_EQ(s.cr_field(3), 0u);
}

TEST(e200_spe_has_no_infinities_nans_or_denormals) {
    uint32_t fscr = 0;
    // overflow: ±max, FOVF
    CHECK_EQ(spe::mul(f32(3e38f), f32(10.0f), &fscr), 0x7F7FFFFFu);
    CHECK((fscr & kSpefscrFovf) && (fscr & kSpefscrFovfs));
    // an infinite operand: invalid, ±max
    fscr = 0;
    CHECK_EQ(spe::add(0xFF800000, f32(1.0f), &fscr), 0xFF7FFFFFu);
    CHECK((fscr & kSpefscrFinv) != 0);
    // a denormal operand is zero
    fscr = 0;
    CHECK_EQ(spe::add(0x00000001, f32(1.0f), &fscr), f32(1.0f));
    // underflow: ±0, FUNF
    fscr = 0;
    CHECK_EQ(spe::mul(f32(1e-30f), f32(1e-30f), &fscr), 0u);
    CHECK((fscr & kSpefscrFunf) != 0);
    // divide by zero: ±max, FDBZ; the status bits are this operation's, the sticky ones accumulate
    CHECK_EQ(spe::div(f32(-1.0f), 0, &fscr), 0xFF7FFFFFu);
    CHECK((fscr & kSpefscrFdbz) && !(fscr & kSpefscrFunf) && (fscr & kSpefscrFunfs));
    // conversions saturate; NaN is 0
    fscr = 0;
    CHECK_EQ(spe::to_signed(f32(3e9f), false, &fscr), 0x7FFFFFFFu);
    CHECK_EQ(spe::to_unsigned(f32(-5.0f), true, &fscr), 0u);
    CHECK_EQ(spe::to_signed(0x7FC00000, false, &fscr), 0u);
    // fractions
    CHECK_EQ(spe::from_signed_fraction(int32_t(0x40000000), &fscr), f32(0.5f));
    CHECK_EQ(spe::to_signed_fraction(f32(-0.25f), &fscr), 0xE0000000u);
}

TEST(e200_spe_rounding_mode_comes_from_spefscr) {
    uint32_t fscr = 1;  // toward zero
    const uint32_t third = spe::div(f32(1.0f), f32(3.0f), &fscr);
    fscr = 2;  // toward +inf
    const uint32_t third_up = spe::div(f32(1.0f), f32(3.0f), &fscr);
    CHECK_EQ(third_up, third + 1);
    CHECK((fscr & kSpefscrFx) != 0);
}

TEST(e200_decode_and_disassemble) {
    CHECK(std::string(disassemble(decode(0x38610008), 0)) == "addi r3,r1,8");
    CHECK(std::string(disassemble(decode(0x4E800020), 0)) == "bclr 20,0");
    CHECK(std::string(disassemble(decode(0x7C6802A6), 0)) == "mfspr r3,8");
    CHECK(std::string(disassemble(decode(0x4BFFFFF0), 0x100)) == "b 0xf0");
    CHECK(std::string(disassemble(decode(0x7C64292D), 0)) == "stwcx. r3,r4,r5");
    CHECK(std::string(disassemble(decode(0x10A322C0), 0)) == "efsadd r5,r3,r4");
    CHECK(decode(0x10A32200).op == Op::Illegal);  // evaddw: the vector forms are not implemented
    CHECK(decode(0xFC000000).op == Op::Illegal);  // classic floating point
    CHECK(decode(0x7C64291E).op == Op::Isel);
    CHECK(ends_block(decode(0x44000002)));
    CHECK(!ends_block(decode(0x38610008)));
}

TEST(e200_spe_round_to_nearest_matches_ieee_and_its_flags) {
    // The round-to-nearest path computes without the host's floating-point state; it must give IEEE's result and
    // inexact flag wherever the SPE agrees with IEEE (finite normal operands and results).
    std::mt19937 rng(7);
    auto random_float = [&rng]() {
        // spread over the exponent range, both signs, some exact small integers
        if (rng() % 8 == 0) {
            return float(int(rng() % 2001) - 1000);
        }
        const uint32_t exp = 1 + rng() % 253;
        return std::bit_cast<float>((rng() & 0x807FFFFFu) | (exp << 23));
    };
    int mismatches = 0;
    for (int i = 0; i < 200000; ++i) {
        const float x = random_float();
        const float y = random_float();
        for (int op = 0; op < 4; ++op) {
            std::feclearexcept(FE_ALL_EXCEPT);
            volatile float vx = x, vy = y;
            volatile float want = op == 0 ? vx + vy : op == 1 ? vx - vy : op == 2 ? vx * vy : vx / vy;
            const bool want_inexact = std::fetestexcept(FE_INEXACT) != 0;
            const float w = want;
            if (!std::isfinite(w) || (w != 0 && std::fabs(w) < 0x1p-126f) || (op == 3 && y == 0)) {
                continue;  // where the SPE differs from IEEE on purpose
            }
            uint32_t fscr = 0;
            const uint32_t got = op == 0 ? spe::add(f32(x), f32(y), &fscr)
                                 : op == 1 ? spe::sub(f32(x), f32(y), &fscr)
                                 : op == 2 ? spe::mul(f32(x), f32(y), &fscr)
                                           : spe::div(f32(x), f32(y), &fscr);
            const bool got_inexact = (fscr & kSpefscrFx) != 0;
            if ((got != f32(w) && !(w == 0 && (got & 0x7FFFFFFF) == 0)) || got_inexact != want_inexact) {
                if (mismatches++ < 5) {
                    std::fprintf(stderr, "op %d: %a %a -> %08x (inexact %d), IEEE %08x (inexact %d)\n", op, double(x),
                                 double(y), got, got_inexact, f32(w), want_inexact);
                }
            }
        }
    }
    CHECK_EQ(mismatches, 0);
}
