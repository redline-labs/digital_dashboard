// The whole system on the real application (skipped without the image): it boots into normal mode on the synthetic
// images, the delivery scenario passes, cut runs equal uncut ones, the engines agree, snapshots replay identically.
#include <cstring>

#include "firmware_path.h"
#include "mctest/test.h"
#include "pdm32/top/instance.h"
#include "pdm32/top/scenario.h"
#include "pdm32/top/sim_session.h"

using namespace sim;
using namespace sim::sys;

namespace {

const std::string kScenario = std::string(PDM32_SCENARIO_DIR) + "/short_output.json";

std::unique_ptr<Instance> make(CpuEngine engine = kDefaultCpuEngine) {
    Scenario s = load_scenario(kScenario);
    InstanceConfig cfg = instance_config_for(s, sim::test::firmware_hex());
    cfg.engine = engine;
    auto in = std::make_unique<Instance>(cfg);
    prepare_scenario(*in, s);
    return in;
}

}  // namespace

TEST(system_boots_into_normal_mode_and_streams) {
    if (sim::test::firmware_hex().empty()) {
        mctest::skip("no firmware image");
        return;
    }
    auto in = make();
    in->run_until_seconds(0.5);
    CHECK_EQ(in->m().mem.bytes[0x02FA], 0);  // run mode normal: config and cal CRCs accepted
    CHECK_EQ(in->m().reset_count(), 1);      // no COP resets
    CHECK(in->board.soc.mscan->frames_sent() > 20);
    CHECK(in->can_node.signals.count("PDM_Output_Status_1") == 1);
    CHECK(in->can_node.signals.count("PDM_Battery_Voltage") == 1);
}

TEST(system_short_output_scenario_passes) {
    if (sim::test::firmware_hex().empty()) {
        mctest::skip("no firmware image");
        return;
    }
    Scenario s = load_scenario(kScenario);
    auto in = make();
    const ScenarioResult r = run_scenario(*in, s);
    if (!r.passed()) {
        std::fputs(format_report(s, r).c_str(), stderr);
    }
    CHECK(r.passed());
}

TEST(system_cut_runs_equal_one_run) {
    if (sim::test::firmware_hex().empty()) {
        mctest::skip("no firmware image");
        return;
    }
    auto whole = make();
    auto cut = make();
    whole->run_until_seconds(1.5);
    for (double t = 0.0137; t < 1.5; t += 0.0137) {
        cut->run_until_seconds(t);
    }
    cut->run_until_seconds(1.5);
    CHECK_EQ(cut->m().cpu.cycles, whole->m().cpu.cycles);
    CHECK(std::memcmp(cut->m().mem.bytes.data(), whole->m().mem.bytes.data(), 0x10000) == 0);
    CHECK_EQ(cut->can_node.logged_total, whole->can_node.logged_total);
}

TEST(system_interpreter_and_block_engine_agree) {
    if (sim::test::firmware_hex().empty()) {
        mctest::skip("no firmware image");
        return;
    }
    auto a = make(CpuEngine::Interpreter);
    auto b = make(CpuEngine::Block);
    a->run_until_seconds(2.5);
    b->run_until_seconds(2.5);
    CHECK_EQ(a->m().cpu.cycles, b->m().cpu.cycles);
    CHECK_EQ(a->m().cpu.pc, b->m().cpu.pc);
    CHECK(std::memcmp(a->m().mem.bytes.data(), b->m().mem.bytes.data(), 0x10000) == 0);
}

TEST(system_interpreter_and_jit_agree) {
    if (sim::test::firmware_hex().empty()) {
        mctest::skip("no firmware image");
        return;
    }
    auto a = make(CpuEngine::Interpreter);
    auto b = make(CpuEngine::Jit);
    // through the short at 2 s: device-heavy ISRs, the logic engine's JSR ,X into FLASH-2, the fault path
    for (double t = 0.25; t <= 2.75; t += 0.25) {
        a->run_until_seconds(t);
        b->run_until_seconds(t);
        CHECK_EQ(a->m().cpu.cycles, b->m().cpu.cycles);
        CHECK_EQ(a->m().cpu.pc, b->m().cpu.pc);
        CHECK(std::memcmp(a->m().mem.bytes.data(), b->m().mem.bytes.data(), 0x10000) == 0);
        CHECK_EQ(a->can_node.logged_total, b->can_node.logged_total);
    }
    if (b->m().jit() != nullptr) {
        CHECK(b->m().jit()->stats.blocks_run > 1000);
    }
}

TEST(system_jit_cut_runs_equal_one_run) {
    if (sim::test::firmware_hex().empty()) {
        mctest::skip("no firmware image");
        return;
    }
    auto whole = make(CpuEngine::Jit);
    auto cut = make(CpuEngine::Jit);
    whole->run_until_seconds(1.2);
    for (double t = 0.0071; t < 1.2; t += 0.0071) {
        cut->run_until_seconds(t);
    }
    cut->run_until_seconds(1.2);
    CHECK_EQ(cut->m().cpu.cycles, whole->m().cpu.cycles);
    CHECK(std::memcmp(cut->m().mem.bytes.data(), whole->m().mem.bytes.data(), 0x10000) == 0);
}

TEST(system_snapshot_restore_replays_identically) {
    if (sim::test::firmware_hex().empty()) {
        mctest::skip("no firmware image");
        return;
    }
    Scenario s = load_scenario(kScenario);
    SessionOptions o;
    o.instance = instance_config_for(s, sim::test::firmware_hex());
    o.scenario = s;
    SimSession session(o);
    session.run_until(1.8);
    session.snapshot("before_short");
    session.run_until(2.6);
    const uint64_t cycles = session.instance().m().cpu.cycles;
    const uint8_t status = session.instance().m().mem.bytes[0x0293];
    CHECK((status & 0x02) != 0);
    CHECK(session.restore("before_short"));
    CHECK_EQ(session.instance().m().mem.bytes[0x0293] & 0x0F, 1);
    session.run_until(2.6);
    CHECK_EQ(session.instance().m().cpu.cycles, cycles);
    CHECK_EQ(session.instance().m().mem.bytes[0x0293], status);
}
