// The engines' contract on hand-picked programs: the block engine (decode cache) runs exactly as the interpreter,
// including after code is rewritten under it. (Random programs: tools/sim_check.cpp --engine-diff.)
#include "flat_env.h"
#include "mctest/test.h"
#include "hc08/block_engine.h"
#include "hc08/cpu.h"

using namespace sim;
using sim::test::FlatEnv;

namespace {

// a loop summing $0200..$020F into A, then WAIT
void load_sum_loop(FlatEnv& env) {
    env.load(0x8000, {0x45, 0x02, 0x00,  // LDHX #$0200
                      0x4F,              // CLRA
                      0xA6, 0x10,        // LDA #16 -> (overwritten below: count in $40)
                      0xB7, 0x40,        // STA $40
                      0x4F,              // CLRA
                      0xFB,              // ADD ,X
                      0xAF, 0x01,        // AIX #1
                      0x3B, 0x40, 0xFA,  // DBNZ $40,-6
                      0x8F});            // WAIT
    for (size_t k = 0; k < 16; k++) {
        env.mem[0x0200 + k] = uint8_t(k + 1);
    }
}

}  // namespace

TEST(engines_block_matches_interpreter_on_a_loop) {
    FlatEnv a;
    FlatEnv b;
    load_sum_loop(a);
    load_sum_loop(b);
    CpuState sa;
    CpuState sb;
    sa.pc = sb.pc = 0x8000;
    sa.ccr = sb.ccr = ccr::kOnes | ccr::kI;

    DecodeCache cache;
    const Stop stop_a = interpret(sa, a);
    const Stop stop_b = run_blocks(sb, b, cache);

    CHECK(stop_a.kind == StopKind::Wait);
    CHECK(stop_b.kind == StopKind::Wait);
    CHECK_EQ(sa.a, 136);
    CHECK_EQ(sb.a, sa.a);
    CHECK_EQ(sb.cycles, sa.cycles);
    CHECK_EQ(sb.pc, sa.pc);
    CHECK_EQ(b.reads, a.reads);
    CHECK_EQ(b.writes, a.writes);
}

TEST(engines_decode_cache_invalidation) {
    FlatEnv env;
    env.load(0x0300, {0xA6, 0x01, 0x8F});  // LDA #1 ; WAIT  (code in RAM)
    DecodeCache cache;
    CpuState s;
    s.pc = 0x0300;
    s.ccr = ccr::kOnes | ccr::kI;
    run_blocks(s, env, cache);
    CHECK_EQ(s.a, 1);
    CHECK(cache.has_code(0x0300));

    env.mem[0x0301] = 0x02;  // patch the immediate: without invalidation the stale decode would load 1 again
    cache.invalidate(0x0301);
    s.pc = 0x0300;
    run_blocks(s, env, cache);
    CHECK_EQ(s.a, 2);
    CHECK_EQ(cache.invalidations(), 1);
}
