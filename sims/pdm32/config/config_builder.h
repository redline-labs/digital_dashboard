// Flash images for the PDM application: a synthetic configuration (format 9, with compiled logic routines), the
// current calibration (compiled in from a real unit's dump, config/current_cal.h) and a synthetic factory block.
// They stand in for what PDM Manager downloads and what the factory programs.
//
// A logic routine is called with JSR ,X, reads channels through the channel-table pointer (layout::kZpChannelTable),
// may set its debounce limit (layout::kZpDebounceHi/Lo), and returns the new state in A (bit 7 set: no result). That
// calling convention is inferred from the caller, not checked against PDM Manager's compiler output.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pdm32/config/current_cal.h"

namespace pdm::config {

// "Output `output` is on while input `input` is above `threshold_v`."
struct OutputRule {
    size_t output = 1;  // 1..32
    size_t input = 1;   // 1..23
    double threshold_v = 6.0;
    int retries = 0;  // 0..254; -1: unlimited
    double retry_delay_s = 1.0;
    double i2t_scale = 0;  // 0: no I^2t (overcurrent) protection on the output
    int i2t_divisor = 10;  // the I^2t filter's divisor (time constant ~1.6 s per unit)
};

inline constexpr uint16_t kDefaultSerial = 1;  // the simulated unit's serial number

struct ConfigSpec {
    std::vector<OutputRule> rules;
    uint32_t stream_base_id = 0x500;  // the "PDM Generic Output" data stream
    int stream_rate_10ms = 1;
    uint8_t stream_enable_mask = 0x7F;
    std::string current_cal = kDefaultCurrentCal;  // a compiled-in calibration (builtin_current_cal)
};

// {"builder": "input_to_output", "input": 1, "output": 1, "threshold_v": 6, ...} or {"rules": [ {...}, ... ]}
ConfigSpec parse_config_spec(const nlohmann::json& j);

// The configuration block, to be placed at layout::kConfig.
std::vector<uint8_t> build_config(const ConfigSpec& spec);

// The current calibration block for a compiled-in calibration (config/current_cal.h), at layout::kCurrentCal.
std::vector<uint8_t> build_current_cal(const std::string& name = kDefaultCurrentCal);

// The factory block for a PDM32 (legacy layout), at layout::kFactory. `serial` is the number the firmware derives and
// reports (on the 0x505 stream and in the comms beacon).
std::vector<uint8_t> build_factory(uint16_t serial = kDefaultSerial);

// The logic channel the rule with index `k` uses (the output's driving channel is 8 + this).
int rule_logic_channel(int k);

}  // namespace pdm::config
