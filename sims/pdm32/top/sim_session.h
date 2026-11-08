// The in-process driver of one simulated PDM: what the agent methods serve and what a test calls directly. Time only moves inside run/step/finish; a run stops at a breakpoint or a
// stopping watch and says why.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "pdm32/top/control.h"
#include "pdm32/top/debugger.h"
#include "pdm32/top/instance.h"
#include "pdm32/top/scenario.h"

namespace sim::sys {

struct SessionOptions {
    InstanceConfig instance;
    std::optional<Scenario> scenario;  // prepared on the instance (harness, events)
    bool allow_write = false;
};

struct RunResult {
    std::string reason;  // "time" (reached), "break", "watch", "exit"
    double t = 0;
    uint16_t pc = 0;
    std::string where;
    std::optional<int> id;  // the breakpoint or watch

    json to_json() const;
};

class SimSession {
   public:
    explicit SimSession(SessionOptions options);

    Instance& instance() {
        return *instance_;
    }
    Debugger& debugger() {
        return *debugger_;
    }
    bool allow_write() const {
        return options_.allow_write;
    }

    RunResult run_until(double t_seconds);
    RunResult run_for(double seconds);
    // `count` instructions; `over` runs a JSR/BSR to its return
    RunResult step(int count, bool over);
    // runs until the current function returns (an RTS/RTI that takes SP above its value now)
    RunResult finish();
    // power-on reset of the PDM (the harness, breakpoints and watches stay)
    void reset();

    void snapshot(const std::string& name);
    bool restore(const std::string& name);
    json snapshot_names() const;

    void can_send(const CanFrame& frame);

    json status();
    json outputs();
    json inputs();

   private:
    RunResult run_to_tick(uint64_t target);
    RunResult result(const std::string& reason, std::optional<int> id = std::nullopt);
    bool step_one();

    struct Saved {
        Machine::Snapshot machine;
        Stimulus::State stimulus;
        plant::Harness harness;
        std::map<std::string, ExternalCanNode::Value> signals;
    };

    SessionOptions options_;
    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Debugger> debugger_;
    std::map<std::string, Saved> snapshots_;
};

// The output status byte in words ("on", "fault", "overcurrent", "retries done", "off").
std::string describe_output_status(uint8_t status);

}  // namespace sim::sys
