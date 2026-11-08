// The debugger: breakpoints by function or pc, watches on named memory, conditions, record-only actions, records, a
// heuristic backtrace. It sits on the machine's pc watches and data watches; the
// session decides what a stop means.
//
// Conditions are "NAME OP VALUE" terms joined by "&&" (OP: == != < <= > >=; NAME as in top/probe.h).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pdm32/top/instance.h"

namespace sim::sys {

class DebugError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

class Debugger {
   public:
    explicit Debugger(Instance& instance) : in_(instance) {
    }

    enum class Action : uint8_t { Stop, Record };
    enum class On : uint8_t { Write, Read, Change };

    struct Breakpoint {
        int id = 0;
        uint16_t pc = 0;
        std::string where;
        std::string condition;
        Action action = Action::Stop;
        std::vector<std::string> record;
        int ignore = 0;
        int hits = 0;
    };

    struct Watch {
        int id = 0;
        std::string name;
        uint16_t begin = 0;
        uint16_t end = 0;
        On on = On::Write;
        std::string condition;
        Action action = Action::Stop;
        std::vector<std::string> record;
        int hits = 0;
        int machine_id = 0;
        std::vector<uint8_t> shadow;  // Change: the last values seen
    };

    struct Record {
        int id;
        double t;
        uint16_t pc;
        std::string where;
        nlohmann::json values;
    };

    // Throws DebugError for an unknown function/name or a malformed condition.
    int add_break(const std::string& function_or_pc, const std::string& condition, Action action,
                  std::vector<std::string> record, int ignore);
    int add_watch(const std::string& name, On on, const std::string& condition, Action action,
                  std::vector<std::string> record);
    bool clear(int id);
    void clear_all();
    nlohmann::json list() const;
    nlohmann::json records(int id, size_t since, size_t limit) const;

    // The machine stopped at a watched pc: true if the run should stop there (the condition holds and the action is
    // stop); otherwise the hit is counted/recorded and the session resumes past it.
    bool on_pc(uint16_t pc);

    // Why the last data-watch break happened (set when a stopping watch fired).
    std::optional<int> take_watch_hit() {
        return std::exchange(last_watch_hit_, std::nullopt);
    }
    std::optional<int> last_breakpoint() const {
        return last_breakpoint_;
    }

    bool condition_holds(const std::string& condition);

    // Return addresses on the stack (heuristic: words after a JSR/BSR), innermost first.
    nlohmann::json backtrace();

    std::string where(uint16_t pc) const;

   private:
    void record(int id, uint16_t pc, const std::vector<std::string>& names);
    void validate_condition(const std::string& condition);

    Instance& in_;
    std::vector<Breakpoint> breakpoints_;
    std::vector<Watch> watches_;
    std::vector<Record> records_;
    int next_id_ = 1;
    std::optional<int> last_watch_hit_;
    std::optional<int> last_breakpoint_;
};

}  // namespace sim::sys
