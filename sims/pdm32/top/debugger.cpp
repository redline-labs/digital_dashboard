#include "pdm32/top/debugger.h"

#include <algorithm>
#include <cmath>
#include <regex>

#include "hc08/disasm.h"
#include "pdm32/top/probe.h"

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

std::string Debugger::where(uint16_t pc) const {
    return in_.symbols.describe(pc);
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
        if (!read_name(in_, m[1].str(), error) && error.find("unknown name") != std::string::npos) {
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
        const auto lhs = read_name(in_, m[1].str(), error);
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
    uint16_t pc;
    if (const auto a = parse_number(nlohmann::json(target)); a && (target[0] == '$' || target.rfind("0x", 0) == 0)) {
        pc = uint16_t(*a);
    } else if (const Symbol* s = in_.symbols.find(target)) {
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
    in_.m().watch_pc(pc, true);
    return b.id;
}

int Debugger::add_watch(const std::string& name, On on, const std::string& condition, Action action,
                        std::vector<std::string> record_names) {
    const auto addr = address_of(in_, name);
    if (!addr) {
        throw DebugError("watch: '" + name + "' is not a memory name (SYMBOL, SYMBOL[k], $addr)");
    }
    validate_condition(condition);

    uint16_t size = 1;
    if (name.find('[') == std::string::npos && name[0] != '$' && name.rfind("0x", 0) != 0) {
        if (const Symbol* s = in_.symbols.find(name)) {
            size = s->size;
        }
    }

    Watch w;
    w.id = next_id_++;
    w.name = name;
    w.begin = *addr;
    w.end = uint16_t(*addr + size);
    w.on = on;
    w.condition = condition;
    w.action = action;
    w.record = std::move(record_names);
    for (uint16_t a = w.begin; a < w.end; a++) {
        w.shadow.push_back(in_.m().mem.bytes[a]);
    }

    const int id = w.id;
    Machine::DataWatch dw;
    dw.begin = w.begin;
    dw.end = w.end;
    dw.on_read = on == On::Read;
    dw.on_write = on != On::Read;
    dw.fn = [this, id](uint16_t a, uint8_t value, bool) {
        auto it = std::find_if(watches_.begin(), watches_.end(), [id](const Watch& x) { return x.id == id; });
        if (it == watches_.end()) {
            return false;
        }
        if (it->on == On::Change) {
            uint8_t& old = it->shadow[a - it->begin];
            if (old == value) {
                return false;
            }
            old = value;
        }
        if (!condition_holds(it->condition)) {
            return false;
        }
        it->hits++;
        this->record(it->id, in_.m().cpu.pc, it->record);
        if (it->action == Action::Stop) {
            last_watch_hit_ = it->id;
            return true;
        }
        return false;
    };
    w.machine_id = in_.m().add_data_watch(std::move(dw));
    watches_.push_back(std::move(w));
    return id;
}

bool Debugger::clear(int id) {
    for (auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
        if (it->id == id) {
            const uint16_t pc = it->pc;
            breakpoints_.erase(it);
            const bool still =
                std::any_of(breakpoints_.begin(), breakpoints_.end(), [pc](const Breakpoint& b) { return b.pc == pc; });
            in_.m().watch_pc(pc, still);
            return true;
        }
    }
    for (auto it = watches_.begin(); it != watches_.end(); ++it) {
        if (it->id == id) {
            in_.m().remove_data_watch(it->machine_id);
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

void Debugger::record(int id, uint16_t pc, const std::vector<std::string>& names) {
    nlohmann::json values = nlohmann::json::object();
    for (const std::string& n : names) {
        std::string error;
        const auto v = read_name(in_, n, error);
        values[n] = v ? nlohmann::json(*v) : nlohmann::json(nullptr);
    }
    records_.push_back({id, in_.seconds(), pc, where(pc), std::move(values)});
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

bool Debugger::on_pc(uint16_t pc) {
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
    Machine& m = in_.m();
    nlohmann::json frames = nlohmann::json::array();
    frames.push_back({{"pc", m.cpu.pc}, {"where", where(m.cpu.pc)}});

    const uint16_t top = std::max<uint16_t>(m.cpu.sp, 0x0934);
    for (uint32_t a = uint32_t(m.cpu.sp) + 1; a + 1 <= top && frames.size() < 16; a++) {
        const uint16_t ret = uint16_t((uint16_t(m.mem.bytes[a]) << 8) | m.mem.bytes[a + 1]);
        if (ret < 0x8000 && !(ret >= 0x1E20 && ret < 0x8000)) {
            continue;
        }
        const auto byte = [&m](uint16_t x) { return m.mem.bytes[x]; };
        const bool after_call = byte(uint16_t(ret - 3)) == 0xCD || byte(uint16_t(ret - 3)) == 0xDD ||
                                byte(uint16_t(ret - 2)) == 0xAD || byte(uint16_t(ret - 2)) == 0xBD ||
                                byte(uint16_t(ret - 2)) == 0xED || byte(uint16_t(ret - 1)) == 0xFD;
        if (!after_call) {
            continue;
        }
        frames.push_back({{"pc", ret}, {"where", where(ret)}, {"stack", a}});
        a++;
    }
    return frames;
}

}  // namespace sim::sys
