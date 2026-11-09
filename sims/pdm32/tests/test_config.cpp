// The synthetic flash images: the CRC, the configuration block's layout and its logic routines (run on the CPU core
// against a fake channel table), the calibration and the factory block.
#include "pdm32/config/current_cal_dumped.h"
#include "pdm32/config/config_builder.h"
#include "pdm32/config/crc16.h"
#include "pdm32/config/layout.h"
#include "flat_env.h"
#include "mctest/test.h"
#include "hc08/cpu.h"

using namespace pdm::config;
namespace layout = pdm::layout;

TEST(config_crc16_ccitt_false_check_value) {
    const std::string text = "123456789";
    CHECK_EQ(crc16_ccitt(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size())), 0x29B1);
}

TEST(config_block_header_length_and_crc) {
    ConfigSpec spec;
    spec.rules.push_back(OutputRule{});
    const std::vector<uint8_t> block = build_config(spec);
    CHECK_EQ(block[0], 0);
    CHECK_EQ(block[1], 9);
    CHECK_EQ((block[2] << 8) | block[3], block.size());
    const uint16_t crc = crc16_ccitt(std::span<const uint8_t>(block.data(), block.size() - 2));
    CHECK_EQ(block[block.size() - 2], crc >> 8);
    CHECK_EQ(block[block.size() - 1], crc & 0xFF);
    // output 1 follows logic channel 1 (channel index 9), mask 1
    const size_t rec = layout::kOutputConfig - layout::kConfig;
    CHECK_EQ((block[rec] << 8) | block[rec + 1], 9);
    CHECK_EQ(block[rec + layout::kOutMask], 1);
    CHECK_EQ(block[layout::kLogicCount - layout::kConfig], 2);
    CHECK_EQ(block[layout::kStreamIdrs - layout::kConfig], 0x500 >> 3);
}

// The unused channels as PDM Manager lays them out (and as a real unit with no logic holds them): one more step past
// the last routine to a CLRA; RTS at the end of the code, then lengths of 0, so every channel from the count on runs
// it. Unused references point at the first of those channels.
TEST(config_unused_channels_run_the_clra_rts_routine) {
    ConfigSpec spec;
    spec.rules.push_back(OutputRule{});
    const std::vector<uint8_t> block = build_config(spec);
    const size_t lengths = layout::kLogicCodeLengths - layout::kConfig;
    const size_t count = block[layout::kLogicCount - layout::kConfig];
    CHECK_EQ(count, 2u);
    CHECK_EQ(block[lengths], 0);
    size_t code = 0;
    for (size_t i = 1; i <= count; i++) {
        CHECK(block[lengths + i] != 0);
        code += block[lengths + i];
    }
    for (size_t i = count + 1; i < layout::kLogicChannels; i++) {
        CHECK_EQ(block[lengths + i], 0);
    }
    const size_t stub = layout::kCompiledLogic - layout::kConfig + code;
    CHECK_EQ(block[stub], 0x4F);
    CHECK_EQ(block[stub + 1], 0x81);
    CHECK_EQ(stub + 2 + 2, block.size());  // then only the CRC
    // an unused output follows the zero channel, index 8 + count
    const size_t rec2 = layout::kOutputConfig + layout::kOutputConfigBytes - layout::kConfig;
    CHECK_EQ((block[rec2] << 8) | block[rec2 + 1], 8 + count);
}

TEST(config_too_many_rules_is_refused) {
    ConfigSpec spec;
    spec.rules.resize(layout::kLogicChannels - 2);  // channel 0, 198 rules, and the zero channel: all 200
    CHECK_EQ(build_config(spec)[layout::kLogicCount - layout::kConfig], 199);
    spec.rules.push_back(OutputRule{});
    bool threw = false;
    try {
        build_config(spec);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

namespace {

// runs logic channel `channel`'s routine as the firmware would: JSR ,X at the compiled logic + the lengths up to it
// (`sets_debounce` false for the unused-channel routine, which leaves it alone)
uint8_t run_routine(const std::vector<uint8_t>& block, size_t channel, uint8_t input1_raw, bool sets_debounce = true) {
    sim::test::FlatEnv env;
    for (size_t k = 0; k < block.size(); k++) {
        env.mem[layout::kConfig + k] = block[k];
    }
    env.mem[0x40] = layout::kChannelTable >> 8;
    env.mem[0x41] = layout::kChannelTable & 0xFF;
    env.mem[layout::kInputVoltage] = input1_raw;
    env.mem[0x42] = 0x55;  // the routine must set its debounce limit
    uint16_t at = layout::kCompiledLogic;
    for (size_t i = 0; i <= channel; i++) {
        at = uint16_t(at + env.mem[layout::kLogicCodeLengths + i]);
    }
    // a caller: JSR ,X ; WAIT
    env.load(0x9000, {0xFD, 0x8F});
    sim::CpuState s;
    s.pc = 0x9000;
    s.hx = at;
    s.sp = 0x00FF;
    s.ccr = sim::ccr::kOnes | sim::ccr::kI;
    const sim::Stop stop = sim::interpret(s, env);
    CHECK(stop.kind == sim::StopKind::Wait);
    CHECK_EQ(env.mem[0x42], sets_debounce ? 0 : 0x55);
    return s.a;
}

}  // namespace

TEST(config_logic_routines_compare_the_input) {
    ConfigSpec spec;
    spec.rules.push_back(OutputRule{.output = 1, .input = 1, .threshold_v = 6.0});
    const std::vector<uint8_t> block = build_config(spec);
    CHECK_EQ(run_routine(block, 0, 0), 1);    // the constant 1 (CAN transmit enable)
    CHECK_EQ(run_routine(block, 1, 29), 0);   // 5.8 V
    CHECK_EQ(run_routine(block, 1, 30), 0);   // 6.0 V: not above
    CHECK_EQ(run_routine(block, 1, 31), 1);   // 6.2 V
    CHECK_EQ(run_routine(block, 2, 200, false), 0);    // the first unused channel: the zero channel
    CHECK_EQ(run_routine(block, 199, 200, false), 0);  // the last
}

TEST(config_current_cal_unity) {
    const std::vector<uint8_t> cal = build_current_cal("unity");
    CHECK_EQ(cal.size(), 4u + 32 * 36 + 2);
    CHECK_EQ(cal[1], 1);
    CHECK_EQ(cal[4 + 17], 17);
    CHECK_EQ(cal[4 + 33], 0x80);
    const uint16_t crc = crc16_ccitt(std::span<const uint8_t>(cal.data(), cal.size() - 2));
    CHECK_EQ((cal[cal.size() - 2] << 8) | cal.back(), crc);
    for (int raw = 0; raw < 256; raw++) {
        CHECK_EQ(unity_current_cal().outputs[5].apply(uint8_t(raw)), raw);
    }
}

TEST(config_current_cal_default_is_the_dumped_unit) {
    // the compiled-in table re-encodes to the dumped block exactly: same length and the dump's CRC
    const std::vector<uint8_t> cal = build_current_cal();
    CHECK_EQ(cal.size(), size_t(0x0486));
    CHECK_EQ((cal[2] << 8) | cal[3], 0x0486);
    CHECK_EQ((cal[cal.size() - 2] << 8) | cal.back(), kCurrentCalDumpedCrc);
    CHECK_EQ(kCurrentCalDumped.outputs[0].gain, 118);
    CHECK_EQ(kCurrentCalDumped.outputs[0].offset, 185);
    CHECK_EQ(kCurrentCalDumped.outputs[3].offset, -74);
}

TEST(config_current_cal_decode_round_trips_and_checks_crc) {
    std::vector<uint8_t> block = build_current_cal();
    const CurrentCal cal = decode_current_cal(block);
    CHECK(encode_current_cal(cal) == block);

    block[100] ^= 1;
    bool threw = false;
    try {
        decode_current_cal(block);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(config_current_cal_sense_inverse) {
    // the raw count the sense hardware must produce calibrates back to the wanted count (to within the cal's steps),
    // except past full scale, where the ADC saturates at 255 (on some outputs from about 225 counts, as on the unit)
    for (const OutputCurrentCal& o : kCurrentCalDumped.outputs) {
        const std::array<uint8_t, 256> raw = o.raw_for_count();
        for (size_t c = 0; c < 256; c++) {
            const size_t got = o.apply(raw[c]);
            CHECK(raw[c] == 255 ? got <= c : (got >= c && got <= c + 2));
        }
    }
}

TEST(config_current_cal_unknown_name_is_rejected) {
    bool threw = false;
    try {
        parse_config_spec(nlohmann::json{{"current_cal", "sn9999"}});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(parse_config_spec(nlohmann::json{{"current_cal", "unity"}}).current_cal, "unity");
}

TEST(config_factory_block_is_a_pdm32) {
    const std::vector<uint8_t> f = build_factory(1234);
    CHECK_EQ(f[layout::kFactoryModel - layout::kFactory], 0);
    // the firmware's legacy serial: (the first 32-bit word - 0x800) >> 1
    const uint32_t word = (uint32_t(f[0]) << 24) | (uint32_t(f[1]) << 16) | (uint32_t(f[2]) << 8) | f[3];
    CHECK_EQ((word - 0x800) >> 1, 1234u);
    CHECK_EQ(build_factory()[3], 0x02);  // the default, serial 1: 0x00000802
}

TEST(config_spec_rejects_bad_numbers) {
    bool threw = false;
    try {
        parse_config_spec(nlohmann::json{{"output", 33}});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}
