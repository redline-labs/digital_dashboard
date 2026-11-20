// The in-process debugging driver of one simulated device, whatever its CPU: time moves only inside run, step and
// finish; a run stops at a breakpoint or a stopping watch and says why. Each simulator's session owns one, with its
// target, and adds what is its own (a connector, a console).
#pragma once

#include <any>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "host/debug/debugger.h"
#include "host/debug/target.h"

namespace sim::sys {

struct RunResult {
    std::string reason;  // "time" (reached), "break", "watch", "step", "finish", "exit", "fault"
    double t = 0;
    uint64_t pc = 0;
    std::string where;
    std::optional<int> id;  // the breakpoint or watch

    nlohmann::json to_json() const;
};

class DebugSession {
   public:
    explicit DebugSession(DebugTarget& target) : target_(target), debugger_(target) {
    }

    DebugTarget& target() {
        return target_;
    }
    Debugger& debugger() {
        return debugger_;
    }

    RunResult run_until(double t_seconds);
    RunResult run_for(double seconds);
    RunResult run_to_tick(uint64_t tick);
    // `count` instructions; `over` runs a call to its return
    RunResult step(int count, bool over);
    // runs until the current function returns
    RunResult finish();

    // a whole-device snapshot by name (what restore() puts back)
    void snapshot(const std::string& name);
    bool restore(const std::string& name);
    nlohmann::json snapshot_names() const;

    RunResult result(const std::string& reason, std::optional<int> id = std::nullopt);

   private:
    // one instruction; false when a watch or an exit stopped it
    bool step_one(RunResult* stopped);

    DebugTarget& target_;
    Debugger debugger_;
    std::map<std::string, std::any> snapshots_;
};

}  // namespace sim::sys
