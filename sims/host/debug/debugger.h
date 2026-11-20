// The debugger every simulator shares: breakpoints by function or pc, watches on named memory, conditions,
// record-only actions, records, and a backtrace. It sits on the target's pc and data watches; the session
// (host/debug/session.h) decides what a stop means.
//
// Conditions are "NAME OP VALUE" terms joined by "&&" (OP: == != < <= > >=; NAME as in host/debug/names.h).
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "host/debug/target.h"

namespace sim::sys {

class DebugError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

class Debugger {
   public:
    explicit Debugger(DebugTarget& target) : t_(target) {
    }

    enum class Action : uint8_t { Stop, Record };
    enum class On : uint8_t { Write, Read, Change };

    struct Breakpoint {
        int id = 0;
        uint64_t pc = 0;
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
        uint64_t begin = 0;
        uint64_t end = 0;
        On on = On::Write;
        std::string condition;
        Action action = Action::Stop;
        std::vector<std::string> record;
        int hits = 0;
        int target_id = 0;
        std::vector<uint8_t> shadow;  // Change: the last values seen
        // the access last counted (one instruction touching several bytes is one hit)
        uint64_t last_hit_tick = ~uint64_t(0);
        uint64_t last_hit_pc = ~uint64_t(0);
    };

    struct Record {
        int id;
        double t;
        uint64_t pc;
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

    // The target stopped at a watched pc: true if the run should stop there (the condition holds and the action is
    // stop); otherwise the hit is counted/recorded and the session resumes past it.
    bool on_pc(uint64_t pc);

    // Why the last data-watch stop happened (set when a stopping watch fired).
    std::optional<int> take_watch_hit() {
        return std::exchange(last_watch_hit_, std::nullopt);
    }
    std::optional<int> last_breakpoint() const {
        return last_breakpoint_;
    }

    bool condition_holds(const std::string& condition);

    // The pc and the return addresses on the stack, innermost first.
    nlohmann::json backtrace();

    std::string where(uint64_t pc) const;
    // An address in the target's width ("0x1F00", "0x00100094").
    std::string hex(uint64_t address) const;

   private:
    void record(int id, uint64_t pc, const std::vector<std::string>& names);
    void validate_condition(const std::string& condition);

    DebugTarget& t_;
    std::vector<Breakpoint> breakpoints_;
    std::vector<Watch> watches_;
    std::vector<Record> records_;
    int next_id_ = 1;
    std::optional<int> last_watch_hit_;
    std::optional<int> last_breakpoint_;
};

}  // namespace sim::sys
