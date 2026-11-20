#include "host/debug/debugger.h"

#include <algorithm>
#include <cmath>
#include <regex>

#include <cstdio>

#include "host/control.h"
#include "host/debug/names.h"

namespace sim::sys {

namespace {

const std::regex kTerm(R"(^\s*([^\s=!<>]+)\s*(==|!=|<=|>=|<|>)\s*(\S+)\s*$)");

std::vector<std::string> split_and(const std::string& text) {
    std::vector<std::string> terms;
    size_t start = 0;
    while (true) {
        const size_t at = text.find("&&", start);
        terms.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) {
            break;
        }
        start = at + 2;
    }
    return terms;
}

}  // namespace

std::string Debugger::where(uint64_t pc) const {
    return t_.symbols().describe(pc, t_.address_digits());
}

std::string Debugger::hex(uint64_t address) const {
    char text[24];
    std::snprintf(text, sizeof text, "0x%0*llX", t_.address_digits(), static_cast<unsigned long long>(address));
    return text;
}

void Debugger::validate_condition(const std::string& condition) {
    if (condition.empty()) {
        return;
    }
    for (const std::string& term : split_and(condition)) {
        std::smatch m;
        if (!std::regex_match(term, m, kTerm) || !parse_number(nlohmann::json(m[3].str()))) {
            throw DebugError("condition '" + term + "': expected NAME OP NUMBER");
        }
        std::string error;
        if (!read_name(t_, m[1].str(), error) && error.find("unknown name") != std::string::npos) {
            throw DebugError("condition: " + error);
        }
    }
}

bool Debugger::condition_holds(const std::string& condition) {
    if (condition.empty()) {
        return true;
    }
    for (const std::string& term : split_and(condition)) {
        std::smatch m;
        if (!std::regex_match(term, m, kTerm)) {
            return false;
        }
        std::string error;
        const auto lhs = read_name(t_, m[1].str(), error);
        const auto rhs = parse_number(nlohmann::json(m[3].str()));
        if (!lhs || !rhs) {
            return false;
        }
        const std::string op = m[2];
        const bool ok = op == "=="   ? *lhs == *rhs
                        : op == "!=" ? *lhs != *rhs
                        : op == "<"  ? *lhs < *rhs
                        : op == "<=" ? *lhs <= *rhs
                        : op == ">"  ? *lhs > *rhs
                                     : *lhs >= *rhs;
        if (!ok) {
            return false;
        }
    }
    return true;
}

int Debugger::add_break(const std::string& target, const std::string& condition, Action action,
                        std::vector<std::string> record_names, int ignore) {
    uint64_t pc;
    if (const auto a = parse_number(nlohmann::json(target)); a && (target[0] == '$' || target.rfind("0x", 0) == 0)) {
        pc = uint64_t(*a);
    } else if (const Symbol* s = t_.symbols().find(target)) {
        pc = s->address;
    } else {
        throw DebugError("break: unknown function '" + target + "' (names lists them)");
    }
    validate_condition(condition);

    Breakpoint b;
    b.id = next_id_++;
    b.pc = pc;
    b.where = where(pc);
    b.condition = condition;
    b.action = action;
    b.record = std::move(record_names);
    b.ignore = ignore;
    breakpoints_.push_back(b);
    t_.watch_pc(pc, true);
    return b.id;
}

int Debugger::add_watch(const std::string& name, On on, const std::string& condition, Action action,
                        std::vector<std::string> record_names) {
    const auto mem = memory_name(t_, name);
    if (!mem) {
        throw DebugError("watch: '" + name + "' is not a memory name (SYMBOL, SYMBOL[k], $addr)");
    }
    validate_condition(condition);

    uint64_t size = mem->size;
    if (mem->size == 1 && name.find('[') == std::string::npos && name[0] != '$' && name.rfind("0x", 0) != 0) {
        if (const Symbol* s = t_.symbols().find(name)) {
            size = s->size;
        }
    }

    Watch w;
    w.id = next_id_++;
    w.name = name;
    w.begin = mem->address;
    w.end = mem->address + size;
    w.on = on;
    w.condition = condition;
    w.action = action;
    w.record = std::move(record_names);
    w.shadow.resize(size_t(size));
    t_.peek(w.begin, w.shadow.data(), w.shadow.size());

    const int id = w.id;
    auto fn = [this, id](uint64_t a, uint8_t value, bool) {
        auto it = std::find_if(watches_.begin(), watches_.end(), [id](const Watch& x) { return x.id == id; });
        if (it == watches_.end()) {
            return false;
        }
        if (it->on == On::Change) {
            uint8_t& old = it->shadow[size_t(a - it->begin)];
            if (old == value) {
                return false;
            }
            old = value;
        }
        // the other bytes of an access already counted: the same answer, no second hit
        const uint64_t tick = t_.now();
        const uint64_t pc = t_.pc();
        if (tick == it->last_hit_tick && pc == it->last_hit_pc) {
            return it->action == Action::Stop && last_watch_hit_ == it->id;
        }
        if (!condition_holds(it->condition)) {
            return false;
        }
        it->last_hit_tick = tick;
        it->last_hit_pc = pc;
        it->hits++;
        this->record(it->id, pc, it->record);
        if (it->action == Action::Stop) {
            last_watch_hit_ = it->id;
            return true;
        }
        return false;
    };
    w.target_id = t_.add_data_watch(w.begin, w.end, on == On::Read, on != On::Read, std::move(fn));
    watches_.push_back(std::move(w));
    return id;
}

bool Debugger::clear(int id) {
    for (auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
        if (it->id == id) {
            const uint64_t pc = it->pc;
            breakpoints_.erase(it);
            const bool still =
                std::any_of(breakpoints_.begin(), breakpoints_.end(), [pc](const Breakpoint& b) { return b.pc == pc; });
            t_.watch_pc(pc, still);
            return true;
        }
    }
    for (auto it = watches_.begin(); it != watches_.end(); ++it) {
        if (it->id == id) {
            t_.remove_data_watch(it->target_id);
            watches_.erase(it);
            return true;
        }
    }
    return false;
}

void Debugger::clear_all() {
    while (!breakpoints_.empty()) {
        clear(breakpoints_.front().id);
    }
    while (!watches_.empty()) {
        clear(watches_.front().id);
    }
}

nlohmann::json Debugger::list() const {
    nlohmann::json out = nlohmann::json::array();
    for (const Breakpoint& b : breakpoints_) {
        out.push_back({{"id", b.id},
                       {"kind", "break"},
                       {"pc", b.pc},
                       {"where", b.where},
                       {"condition", b.condition},
                       {"action", b.action == Action::Stop ? "stop" : "record"},
                       {"hits", b.hits}});
    }
    for (const Watch& w : watches_) {
        out.push_back({{"id", w.id},
                       {"kind", "watch"},
                       {"name", w.name},
                       {"address", w.begin},
                       {"size", w.end - w.begin},
                       {"on", w.on == On::Write  ? "write"
                              : w.on == On::Read ? "read"
                                                 : "change"},
                       {"condition", w.condition},
                       {"action", w.action == Action::Stop ? "stop" : "record"},
                       {"hits", w.hits}});
    }
    return out;
}

void Debugger::record(int id, uint64_t pc, const std::vector<std::string>& names) {
    nlohmann::json values = nlohmann::json::object();
    for (const std::string& n : names) {
        std::string error;
        const auto v = read_name(t_, n, error);
        values[n] = v ? nlohmann::json(*v) : nlohmann::json(nullptr);
    }
    records_.push_back({id, t_.seconds(), pc, where(pc), std::move(values)});
    if (records_.size() > 100'000) {
        records_.erase(records_.begin(), records_.begin() + 10'000);
    }
}

nlohmann::json Debugger::records(int id, size_t since, size_t limit) const {
    nlohmann::json out = nlohmann::json::array();
    size_t index = 0;
    for (const Record& r : records_) {
        if (id != 0 && r.id != id) {
            continue;
        }
        if (index++ < since) {
            continue;
        }
        if (out.size() >= limit) {
            break;
        }
        out.push_back({{"id", r.id}, {"t", r.t}, {"pc", r.pc}, {"where", r.where}, {"values", r.values}});
    }
    return out;
}

bool Debugger::on_pc(uint64_t pc) {
    last_breakpoint_.reset();
    bool stop = false;
    for (Breakpoint& b : breakpoints_) {
        if (b.pc != pc || !condition_holds(b.condition)) {
            continue;
        }
        b.hits++;
        if (b.hits <= b.ignore) {
            continue;
        }
        record(b.id, pc, b.record);
        if (b.action == Action::Stop) {
            stop = true;
            last_breakpoint_ = b.id;
        }
    }
    return stop;
}

nlohmann::json Debugger::backtrace() {
    nlohmann::json frames = nlohmann::json::array();
    frames.push_back({{"pc", t_.pc()}, {"where", where(t_.pc())}});
    for (const uint64_t ret : t_.return_addresses()) {
        frames.push_back({{"pc", ret}, {"where", where(ret)}});
    }
    return frames;
}

}  // namespace sim::sys
