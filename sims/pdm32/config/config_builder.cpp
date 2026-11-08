#include "pdm32/config/config_builder.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "pdm32/config/crc16.h"
#include "hc08/assembler.h"
#include "pdm32/config/layout.h"

namespace pdm::config {

using sim::Hc08Asm;

namespace {

// a routine returning a constant (the CAN-TX-enable channel's 1, the unused channels' 0)
std::vector<uint8_t> constant_routine(uint8_t value) {
    Hc08Asm a;
    a.clr_dir(layout::kZpDebounceHi);
    a.clr_dir(layout::kZpDebounceLo);
    a.lda_imm(value);
    a.rts();
    return a.finish();
}

// 1 while the input's raw voltage (0.2 V/bit) is above the threshold, else 0
std::vector<uint8_t> input_above_routine(size_t input, double threshold_v) {
    const int raw = std::clamp(int(std::lround(threshold_v / 0.2)), 0, 254);
    const uint16_t channel = uint16_t(layout::kInputVoltage - layout::kChannelTable + (input - 1));

    Hc08Asm a;
    const auto on = a.label();
    a.clr_dir(layout::kZpDebounceHi);
    a.clr_dir(layout::kZpDebounceLo);
    a.ldhx_dir(layout::kZpChannelTable);
    a.lda_ix2(channel);
    a.cmp_imm(uint8_t(raw));
    a.bhi(on);
    a.clra();
    a.rts();
    a.bind(on);
    a.lda_imm(1);
    a.rts();
    return a.finish();
}

void put16(std::vector<uint8_t>& image, uint16_t addr, uint16_t value) {
    const size_t at = addr - layout::kConfig;
    image.at(at) = uint8_t(value >> 8);
    image.at(at + 1) = uint8_t(value);
}

void put8(std::vector<uint8_t>& image, uint16_t addr, uint8_t value) {
    image.at(addr - layout::kConfig) = value;
}

void seal(std::vector<uint8_t>& block, uint8_t format) {
    block[0] = 0;
    block[1] = format;
    block[2] = uint8_t(block.size() >> 8);
    block[3] = uint8_t(block.size());
    const uint16_t crc = crc16_ccitt(std::span<const uint8_t>(block.data(), block.size() - 2));
    block[block.size() - 2] = uint8_t(crc >> 8);
    block[block.size() - 1] = uint8_t(crc);
}

}  // namespace

size_t rule_logic_channel(size_t k) {
    return 1 + k;  // channel 0 is the constant 1
}

ConfigSpec parse_config_spec(const nlohmann::json& j) {
    ConfigSpec spec;
    const auto rule_of = [](const nlohmann::json& r) {
        OutputRule rule;
        // Read signed, so a negative number is refused below rather than wrapping into range.
        const int64_t output = r.value("output", int64_t{1});
        const int64_t input = r.value("input", int64_t{1});
        if (output < 1 || static_cast<uint64_t>(output) > layout::kOutputs || input < 1 ||
            static_cast<uint64_t>(input) > layout::kInputs) {
            throw std::runtime_error("config: output must be 1..32 and input 1..23");
        }
        rule.output = static_cast<size_t>(output);
        rule.input = static_cast<size_t>(input);
        rule.threshold_v = r.value("threshold_v", 6.0);
        rule.retries = r.value("retries", 0);
        rule.retry_delay_s = r.value("retry_delay_s", 1.0);
        rule.i2t_scale = r.value("i2t_scale", 0.0);
        rule.i2t_divisor = std::clamp(r.value("i2t_divisor", 10), 1, 255);
        return rule;
    };

    if (j.contains("rules")) {
        for (const auto& r : j.at("rules")) {
            spec.rules.push_back(rule_of(r));
        }
    } else {
        const std::string builder = j.value("builder", "input_to_output");
        if (builder != "input_to_output") {
            throw std::runtime_error("config: unknown builder '" + builder + "'");
        }
        spec.rules.push_back(rule_of(j));
    }
    spec.stream_base_id = j.value("stream_base_id", 0x500u);
    spec.current_cal = j.value("current_cal", std::string(kDefaultCurrentCal));
    builtin_current_cal(spec.current_cal);  // reject an unknown name here
    spec.stream_rate_10ms = j.value("stream_rate_10ms", 1);
    return spec;
}

std::vector<uint8_t> build_config(const ConfigSpec& spec) {
    // the logic: channel 0 = 1 (enables CAN transmission), one channel per rule, then a constant 0 (the channels past
    // the count re-run the last routine, so they read 0)
    std::vector<std::vector<uint8_t>> routines;
    routines.push_back(constant_routine(1));
    for (const OutputRule& rule : spec.rules) {
        routines.push_back(input_above_routine(rule.input, rule.threshold_v));
    }
    routines.push_back(constant_routine(0));
    const size_t count = routines.size();
    if (count > layout::kLogicChannels) {
        // The count is one byte in the block and every channel index past it would wrap.
        throw std::runtime_error("config: " + std::to_string(spec.rules.size()) + " rules need more than " +
                                 std::to_string(layout::kLogicChannels) + " logic channels");
    }
    const size_t zero_channel = 8 + count - 1;

    size_t code_bytes = 0;
    for (const auto& r : routines) {
        code_bytes += r.size();
    }
    const size_t total = (layout::kCompiledLogic - layout::kConfig) + code_bytes + 2;
    if (layout::kConfig + total > layout::kFlash2End) {
        throw std::runtime_error("config: logic does not fit FLASH-2");
    }
    std::vector<uint8_t> image(total, 0);

    // data stream: 0x500..0x505 as standard identifiers, every 10 ms x rate, all groups
    for (size_t k = 0; k < 6; k++) {
        const uint32_t id = spec.stream_base_id + uint32_t(k);
        put8(image, uint16_t(layout::kStreamIdrs + 4 * k), uint8_t(id >> 3));
        put8(image, uint16_t(layout::kStreamIdrs + 4 * k + 1), uint8_t((id & 7) << 5));
    }
    put8(image, layout::kStreamRate, uint8_t(std::max(1, spec.stream_rate_10ms)));
    put8(image, layout::kStreamEnable, spec.stream_enable_mask);

    // outputs: unused ones follow the zero channel with mask 0
    for (size_t k = 0; k < layout::kOutputs; k++) {
        const uint16_t rec = uint16_t(layout::kOutputConfig + layout::kOutputConfigBytes * k);
        put16(image, uint16_t(rec + layout::kOutChannelHi), uint16_t(zero_channel));
        put8(image, uint16_t(rec + layout::kOutMask), 0);
    }
    for (size_t r = 0; r < spec.rules.size(); r++) {
        const OutputRule& rule = spec.rules[r];
        const uint16_t rec = uint16_t(layout::kOutputConfig + layout::kOutputConfigBytes * (rule.output - 1));
        put16(image, uint16_t(rec + layout::kOutChannelHi), uint16_t(8 + rule_logic_channel(r)));
        put8(image, uint16_t(rec + layout::kOutMask), 0x01);
        const int delay = std::clamp(int(std::lround(rule.retry_delay_s / 0.02)), 1, 0xFFFF);
        put16(image, uint16_t(rec + layout::kOutRetryDelayHi), uint16_t(delay));
        put8(image, uint16_t(rec + layout::kOutRetries),
             rule.retries < 0 ? 0xFF : uint8_t(std::min(rule.retries, 254)));
        put16(image, uint16_t(rec + layout::kOutI2tScaleHi), uint16_t(std::clamp(rule.i2t_scale, 0.0, 65535.0)));
        put8(image, uint16_t(rec + layout::kOutI2tDivisor), uint8_t(rule.i2t_divisor));
    }

    // logic channels: lengths are the pointer increments before each call
    for (size_t i = 1; i < count; i++) {
        put8(image, uint16_t(layout::kLogicCodeLengths + i), uint8_t(routines[i - 1].size()));
    }
    put8(image, layout::kLogicCount, uint8_t(count));

    // the 0x500 input bits: a rule's input shows its channel, the others the zero channel
    for (size_t i = 0; i < layout::kInputs; i++) {
        put8(image, uint16_t(layout::kInputStateChannels + i), uint8_t(zero_channel));
    }
    for (size_t r = 0; r < spec.rules.size(); r++) {
        put8(image, uint16_t(layout::kInputStateChannels + spec.rules[r].input - 1),
             uint8_t(8 + rule_logic_channel(r)));
    }

    // fault reset and load shedding off (mask 0); CAN transmission enabled by channel 0
    put16(image, layout::kFaultResetChannel, uint16_t(zero_channel));
    put8(image, layout::kFaultResetChannel + 2, 0);
    put16(image, layout::kOutputShedChannel, uint16_t(zero_channel));
    put8(image, layout::kOutputShedChannel + 2, 0);
    put16(image, layout::kCanTxEnableChannel, 8);
    put8(image, layout::kCanTxEnableChannel + 2, 0x01);

    size_t at = layout::kCompiledLogic - layout::kConfig;
    for (const auto& r : routines) {
        std::copy(r.begin(), r.end(), image.data() + at);
        at += r.size();
    }

    seal(image, layout::kConfigFormat);
    return image;
}

std::vector<uint8_t> build_current_cal(const std::string& name) {
    return encode_current_cal(builtin_current_cal(name));
}

std::vector<uint8_t> build_factory(uint16_t serial) {
    std::vector<uint8_t> block(0x0500 - layout::kFactory, 0xFF);
    const auto put = [&](uint16_t addr, uint8_t v) { block[addr - layout::kFactory] = v; };
    // the legacy serial: the firmware takes (the first 32-bit word - 0x800) >> 1
    const uint32_t word = 2 * uint32_t(serial) + 0x800;
    put(0x0462, uint8_t(word >> 24));
    put(0x0463, uint8_t(word >> 16));
    put(layout::kFactorySerial, uint8_t(word >> 8));
    put(layout::kFactorySerial + 1, uint8_t(word));
    put(layout::kFactoryModel, 0);  // PDM32
    put(0x0469, 0);                 // sub-type
    put(0x046A, 0);                 // hardware-revision gate: 0, so revision 0 (1 Mbit/s)
    put(0x046B, 0);
    return block;
}

}  // namespace pdm::config
