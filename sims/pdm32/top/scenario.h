// Scenarios: a run described as data. JSON, parsed into plain structs here and run
// by run_scenario against an Instance:
//
//   {
//     "name": "...", "comment": "...", "seconds": 3.0, "engine": "block",
//     "battery_v": 13.5,
//     "config": { "builder": "input_to_output", "input": 1, "output": 1, "threshold_v": 6.0, "retries": 0 },
//     "loads":  [ {"output": 1, "load": "6"} ],            ("open", "short", "lamp:2.4", ohms)
//     "inputs": [ {"input": 1, "volts": 0} ],              ("open" or volts)
//     "events": [ {"t": 1.0, "set": "in[1].v", "value": 12.0}, {"t": 2.0, "set": "out[1].load", "value": "short"} ],
//     "expect": [ {"t": 1.2, "read": "out[1].pin", "eq": 1, "why": "..."},
//                 {"after": 2.0, "by": 2.5, "read": "out[1].status", "mask": "0x02", "eq": "0x02"} ],
//     "trace":  { "period_ms": 20, "names": ["in[1].v", "out[1].pin", "out[1].v", "can:PDM_Output_Status_1"] }
//   }
//
// An expectation with "t" is checked once at that time; one with "by" passes the first time it holds in
// ["after", "by"] (checked every millisecond). Comparisons: eq, ne, lt, le, gt, ge (numbers or "0x.." strings),
// with an optional "mask" applied first and "tol" for eq.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hc08/engines.h"
#include "pdm32/top/instance.h"

namespace sim::sys {

struct ScenarioEvent {
    double t = 0;
    std::string name;
    nlohmann::json value;
};

struct Expectation {
    std::optional<double> at;
    double after = 0;
    std::optional<double> by;
    std::string name;
    std::string op;  // eq ne lt le gt ge
    double operand = 0;
    std::optional<uint32_t> mask;
    double tolerance = 0;
    std::string why;
};

struct Scenario {
    std::string name;
    std::string comment;
    std::string path;  // where it was read from (relative config files resolve against its directory)
    double seconds = 1.0;
    std::optional<CpuEngine> engine;
    double battery_v = 13.5;
    nlohmann::json config;
    std::vector<std::pair<int, std::string>> loads;      // output (1-based), load text
    std::vector<std::pair<int, nlohmann::json>> inputs;  // input (1-based), volts or "open"
    std::vector<ScenarioEvent> events;
    std::vector<Expectation> expectations;
    double trace_period_s = 0.02;
    std::vector<std::string> trace_names;
};

// Throws std::runtime_error naming the file and the entry.
Scenario parse_scenario(const nlohmann::json& j, const std::string& origin = "<scenario>");
Scenario load_scenario(const std::string& path);

// The InstanceConfig a scenario asks for (firmware from `firmware_path` or the default).
InstanceConfig instance_config_for(const Scenario& scenario, const std::string& firmware_path);

// Applies the scenario's initial harness and schedules its events on the instance.
void prepare_scenario(Instance& instance, const Scenario& scenario);

struct ExpectationResult {
    const Expectation* expectation = nullptr;
    bool passed = false;
    std::optional<double> value;  // the value at the decision
    double decided_at = 0;
    std::string note;
};

struct ScenarioResult {
    std::vector<ExpectationResult> expectations;
    std::vector<std::string> trace_header;
    std::vector<std::vector<std::string>> trace_rows;
    bool passed() const;
};

// Runs a prepared instance through the scenario's time line, checking the expectations and recording the trace.
ScenarioResult run_scenario(Instance& instance, const Scenario& scenario);

// The report as text: the trace table, then one line per expectation.
std::string format_report(const Scenario& scenario, const ScenarioResult& result);

}  // namespace sim::sys
