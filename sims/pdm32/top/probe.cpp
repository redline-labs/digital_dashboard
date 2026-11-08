#include "pdm32/top/probe.h"

#include "dbc_parser/decode.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <regex>

namespace sim::sys {

namespace {

const std::regex kIndexed(R"(^(out|in)\[(\d+)\]\.(\w+)$)");
const std::regex kSymbolIndex(R"(^([A-Za-z_]\w*)\[(\d+)\]$)");
const std::regex kSymbolWord(R"(^([A-Za-z_]\w*)\.w$)");
const std::regex kDeviceRegister(R"(^([a-z][a-z0-9]*)\.([A-Za-z0-9]+)$)");

// The digits of an indexed name, or 0 (out of every range) when they overflow.
size_t index_of(const std::string& digits) {
    size_t n = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    return ec == std::errc() && end == digits.data() + digits.size() ? n : 0;
}

std::optional<uint16_t> parse_address(const std::string& text) {
    try {
        if (text.size() > 1 && text[0] == '$') {
            return uint16_t(std::stoul(text.substr(1), nullptr, 16));
        }
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            return uint16_t(std::stoul(text.substr(2), nullptr, 16));
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

}  // namespace

std::optional<double> parse_number(const nlohmann::json& value) {
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (auto a = parse_address(s)) {
            return double(*a);
        }
        try {
            size_t used = 0;
            const double d = std::stod(s, &used);
            if (used == s.size()) {
                return d;
            }
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

std::optional<uint16_t> address_of(const Instance& instance, const std::string& name) {
    if (auto a = parse_address(name)) {
        return a;
    }
    std::smatch m;
    if (std::regex_match(name, m, kSymbolIndex)) {
        if (const Symbol* s = instance.symbols.find(m[1].str())) {
            return uint16_t(s->address + std::stoi(m[2]));
        }
        return std::nullopt;
    }
    if (const Symbol* s = instance.symbols.find(name)) {
        return s->address;
    }
    return std::nullopt;
}

std::optional<double> read_name(Instance& in, const std::string& name, std::string& error) {
    Machine& m = in.m();
    const CpuState& c = m.cpu;

    if (name == "t") {
        return m.seconds();
    }
    if (name == "pc") {
        return c.pc;
    }
    if (name == "a") {
        return c.a;
    }
    if (name == "x") {
        return c.x();
    }
    if (name == "h") {
        return c.h();
    }
    if (name == "hx") {
        return c.hx;
    }
    if (name == "sp") {
        return c.sp;
    }
    if (name == "ccr") {
        return c.ccr;
    }
    if (name == "battery") {
        return in.board.battery();
    }

    std::smatch mt;
    if (std::regex_match(name, mt, kIndexed)) {
        const bool output = mt[1] == "out";
        const size_t n = index_of(mt[2]);
        const std::string field = mt[3];
        const size_t count = output ? Pdm32Board::kOutputs : Pdm32Board::kInputs;
        if (n < 1 || n > count) {
            error = name + ": index out of range 1.." + std::to_string(count);
            return std::nullopt;
        }
        const size_t k = n - 1;
        if (output) {
            const HiSideSwitch& sw = in.board.output(k);
            if (field == "pin") {
                return in.board.gate(k) ? 1.0 : 0.0;
            }
            if (field == "v") {
                return sw.output_voltage();
            }
            if (field == "i") {
                return sw.current();
            }
            if (field == "tj") {
                return sw.junction_temperature();
            }
            if (field == "shutdown") {
                return sw.thermal_shutdown() ? 1.0 : 0.0;
            }
            if (field == "load") {
                return in.board.load(k);
            }
            if (field == "status") {
                return m.mem.bytes[0x0293 + k];
            }
        } else {
            if (field == "v") {
                return in.board.input_voltage(k);
            }
            if (field == "raw") {
                return m.mem.bytes[0x01E4 + k];
            }
        }
        error = name + ": unknown field '" + field + "'";
        return std::nullopt;
    }

    if (name.rfind("can:", 0) == 0 || name.rfind("can_age:", 0) == 0) {
        const bool age = name[3] == '_';
        const std::string signal = name.substr(age ? 8 : 4);
        const auto it = in.can_node.signals.find(signal);
        if (it == in.can_node.signals.end()) {
            error = name + ": not received yet" + (dbc_parser::findMessageWithSignal(in.dbc, signal) ? "" : " (and not in the DBC)");
            return std::nullopt;
        }
        if (age) {
            return double(m.now() - it->second.t) / double(m.cfg.tick_hz);
        }
        return it->second.value;
    }

    if (std::regex_match(name, mt, kSymbolWord)) {
        if (const Symbol* s = in.symbols.find(mt[1].str())) {
            return double((uint16_t(m.peek(s->address)) << 8) | m.peek(uint16_t(s->address + 1)));
        }
    }

    if (auto addr = address_of(in, name)) {
        return m.peek(*addr);
    }

    if (std::regex_match(name, mt, kDeviceRegister)) {
        if (Periph* dev = m.find(mt[1].str())) {
            for (const DeviceRegister& r : dev->registers()) {
                if (r.name == mt[2].str()) {
                    dev->sync(m.now());
                    return dev->peek(r.offset);
                }
            }
            error = name + ": " + mt[1].str() + " has no register " + mt[2].str();
            return std::nullopt;
        }
    }

    error = name + ": unknown name";
    return std::nullopt;
}

std::string set_name(Instance& in, const std::string& name, const nlohmann::json& value, bool allow_memory) {
    if (name == "battery") {
        const auto v = parse_number(value);
        if (!v) {
            return "battery: expected volts";
        }
        in.harness.battery_v = *v;
        in.stimulus->apply();
        return "";
    }

    std::smatch mt;
    if (std::regex_match(name, mt, kIndexed)) {
        const bool output = mt[1] == "out";
        const size_t n = index_of(mt[2]);
        const std::string field = mt[3];
        if (output && field == "load" && n >= 1 && n <= Pdm32Board::kOutputs) {
            try {
                const std::string text =
                    value.is_string() ? value.get<std::string>() : std::to_string(value.get<double>());
                in.harness.loads[n - 1] = plant::parse_load(text);
            } catch (const std::exception& e) {
                return e.what();
            }
            in.stimulus->apply();
            return "";
        }
        if (!output && field == "v" && n >= 1 && n <= Pdm32Board::kInputs) {
            plant::InputSource& src = in.harness.inputs[n - 1];
            if (value.is_string() && value.get<std::string>() == "open") {
                src.open = true;
            } else if (const auto v = parse_number(value)) {
                src.open = false;
                src.volts = *v;
            } else {
                return name + ": expected volts or \"open\"";
            }
            in.stimulus->apply();
            return "";
        }
        return name + ": not settable (in[N].v, out[N].load)";
    }

    if (auto addr = address_of(in, name)) {
        if (!allow_memory) {
            return name + ": memory writes need --allow-write";
        }
        const auto v = parse_number(value);
        if (!v || *v < 0 || *v > 255) {
            return name + ": expected a byte";
        }
        in.m().poke(*addr, uint8_t(*v));
        return "";
    }
    return name + ": unknown name";
}

}  // namespace sim::sys
