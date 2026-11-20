#include "pdm32/top/target.h"

#include <charconv>
#include <cmath>
#include <regex>

#include "dbc_parser/decode.h"
#include "hc08/disasm.h"
#include "host/control.h"

namespace sim::sys {

namespace {

const std::regex kIndexed(R"(^(out|in)\[(\d+)\]\.(\w+)$)");
const std::regex kDeviceRegister(R"(^([a-z][a-z0-9]*)\.([A-Za-z0-9]+)$)");

// The digits of an indexed name, or 0 (out of every range) when they overflow.
size_t index_of(const std::string& digits) {
    size_t n = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    return ec == std::errc() && end == digits.data() + digits.size() ? n : 0;
}

// Everything a snapshot holds: the machine and the world around it.
struct Saved {
    Machine::Snapshot machine;
    Stimulus::State stimulus;
    plant::Harness harness;
    std::map<std::string, ExternalCanNode::Value> signals;
};

}  // namespace

double Pdm32Target::seconds() const {
    return in_.m().seconds();
}
uint64_t Pdm32Target::now() const {
    return in_.m().now();
}
uint64_t Pdm32Target::ticks(double s) const {
    return in_.ticks(s);
}

uint64_t Pdm32Target::pc() const {
    return in_.m().cpu.pc;
}
uint64_t Pdm32Target::stack_pointer() const {
    return in_.m().cpu.sp;
}

nlohmann::json Pdm32Target::registers() const {
    const CpuState& c = in_.m().cpu;
    return nlohmann::json{{"pc", c.pc}, {"a", c.a},   {"x", c.x()},     {"h", c.h()},
                          {"hx", c.hx}, {"sp", c.sp}, {"ccr", c.ccr}, {"cycles", c.cycles}};
}

std::optional<uint64_t> Pdm32Target::read_register(const std::string& name) const {
    const CpuState& c = in_.m().cpu;
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
    return std::nullopt;
}

bool Pdm32Target::write_register(const std::string& name, uint64_t v) {
    CpuState& c = in_.m().cpu;
    if (name == "pc") {
        c.pc = uint16_t(v);
    } else if (name == "a") {
        c.a = uint8_t(v);
    } else if (name == "x") {
        c.set_x(uint8_t(v));
    } else if (name == "h") {
        c.set_h(uint8_t(v));
    } else if (name == "hx") {
        c.hx = uint16_t(v);
    } else if (name == "sp") {
        c.sp = uint16_t(v);
    } else if (name == "ccr") {
        c.ccr = uint8_t(v | ccr::kOnes);
    } else {
        return false;
    }
    return true;
}

bool Pdm32Target::peek(uint64_t address, uint8_t* out, size_t size) {
    for (size_t k = 0; k < size; ++k) {
        if (address + k > 0xFFFF) {
            return false;
        }
        out[k] = in_.m().peek(uint16_t(address + k));
    }
    return true;
}

bool Pdm32Target::poke(uint64_t address, const uint8_t* bytes, size_t size) {
    for (size_t k = 0; k < size; ++k) {
        if (address + k > 0xFFFF) {
            return false;
        }
        in_.m().poke(uint16_t(address + k), bytes[k]);
    }
    return true;
}

Decoded Pdm32Target::decode(uint64_t address) {
    Machine& m = in_.m();
    const uint16_t pc = uint16_t(address);
    const Insn insn = sim::decode(pc, [&m](uint16_t a) { return m.mem.bytes[a]; });
    Decoded d;
    d.text = disassemble(insn, pc);
    d.length = insn.length;
    d.call = insn.op == Op::Jsr || insn.op == Op::Bsr;
    d.ret = insn.op == Op::Rts;
    return d;
}

void Pdm32Target::watch_pc(uint64_t pc, bool on) {
    in_.m().watch_pc(uint16_t(pc), on);
}
void Pdm32Target::skip_pc_once(uint64_t pc) {
    in_.m().cpu.skip_watch_pc = int32_t(pc);
}

int Pdm32Target::add_data_watch(uint64_t begin, uint64_t end, bool on_read, bool on_write, DataWatchFn fn) {
    Machine::DataWatch dw;
    dw.begin = uint16_t(begin);
    dw.end = uint16_t(std::min<uint64_t>(end, 0x10000));
    dw.on_read = on_read;
    dw.on_write = on_write;
    dw.fn = [callback = std::move(fn)](uint16_t a, uint8_t value, bool write) { return callback(a, value, write); };
    return in_.m().add_data_watch(std::move(dw));
}
void Pdm32Target::remove_data_watch(int id) {
    in_.m().remove_data_watch(id);
}

namespace {
TargetStop from(const Stop& stop) {
    switch (stop.kind) {
        case StopKind::Watch:
            return {TargetStopKind::PcWatch, stop.pc};
        case StopKind::Break:
            return {TargetStopKind::DataWatch, stop.pc};
        case StopKind::Exit:
            return {TargetStopKind::Exit, stop.pc};
        case StopKind::None:
        case StopKind::Limit:
        case StopKind::Wait:
        case StopKind::StopMode:
        case StopKind::Illegal:
            break;
    }
    return {TargetStopKind::Time, stop.pc};
}
}  // namespace

TargetStop Pdm32Target::run_until(uint64_t tick) {
    return from(in_.m().run_until(tick));
}

TargetStop Pdm32Target::step_instruction(uint64_t max_ticks) {
    Machine& m = in_.m();
    const uint64_t cycles = m.cpu.cycles;
    const uint64_t give_up = m.now() + max_ticks;
    stepped_ = false;
    while (m.cpu.cycles == cycles && m.now() < give_up) {
        const TargetStop stop = from(m.run_until(m.now() + 1));
        if (stop.kind == TargetStopKind::DataWatch || stop.kind == TargetStopKind::Exit) {
            stepped_ = m.cpu.cycles != cycles;
            return stop;
        }
    }
    stepped_ = m.cpu.cycles != cycles;
    return {TargetStopKind::Time, m.cpu.pc};
}

void Pdm32Target::clear_exit() {
    in_.m().clear_exit();
}

void Pdm32Target::reset() {
    in_.power_on();
}

std::vector<uint64_t> Pdm32Target::return_addresses() {
    // words on the stack that follow a JSR/BSR
    Machine& m = in_.m();
    std::vector<uint64_t> out;
    const uint16_t top = std::max<uint16_t>(m.cpu.sp, 0x0934);
    for (uint32_t a = uint32_t(m.cpu.sp) + 1; a + 1 <= top && out.size() < 15; a++) {
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
        out.push_back(ret);
        a++;
    }
    return out;
}

std::any Pdm32Target::snapshot() {
    return Saved{in_.m().snapshot(), in_.stimulus->state(), in_.harness, in_.can_node.signals};
}

void Pdm32Target::restore(const std::any& snapshot) {
    const Saved& s = std::any_cast<const Saved&>(snapshot);
    in_.m().restore(s.machine);
    in_.stimulus->restore(s.stimulus);
    in_.harness = s.harness;
    in_.can_node.signals = s.signals;
}

std::optional<double> Pdm32Target::read_device_name(const std::string& name, std::string& error) {
    Machine& m = in_.m();
    if (name == "battery") {
        return in_.board.battery();
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
            const HiSideSwitch& sw = in_.board.output(k);
            if (field == "pin") {
                return in_.board.gate(k) ? 1.0 : 0.0;
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
                return in_.board.load(k);
            }
            if (field == "status") {
                return m.mem.bytes[0x0293 + k];
            }
        } else {
            if (field == "v") {
                return in_.board.input_voltage(k);
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
        const auto it = in_.can_node.signals.find(signal);
        if (it == in_.can_node.signals.end()) {
            error = name + ": not received yet" +
                    (dbc_parser::findMessageWithSignal(in_.dbc, signal) ? "" : " (and not in the DBC)");
            return std::nullopt;
        }
        if (age) {
            return double(m.now() - it->second.t) / double(m.cfg.tick_hz);
        }
        return it->second.value;
    }

    if (std::regex_match(name, mt, kDeviceRegister)) {
        if (auto* dev = dynamic_cast<Periph*>(m.find(mt[1].str()))) {
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
    return std::nullopt;
}

std::optional<std::string> Pdm32Target::write_device_name(const std::string& name, const nlohmann::json& value) {
    if (name == "battery") {
        const auto v = parse_number(value);
        if (!v) {
            return "battery: expected volts";
        }
        in_.harness.battery_v = *v;
        in_.stimulus->apply();
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
                in_.harness.loads[n - 1] = plant::parse_load(text);
            } catch (const std::exception& e) {
                return std::string(e.what());
            }
            in_.stimulus->apply();
            return "";
        }
        if (!output && field == "v" && n >= 1 && n <= Pdm32Board::kInputs) {
            plant::InputSource& src = in_.harness.inputs[n - 1];
            if (value.is_string() && value.get<std::string>() == "open") {
                src.open = true;
            } else if (const auto v = parse_number(value)) {
                src.open = false;
                src.volts = *v;
            } else {
                return name + ": expected volts or \"open\"";
            }
            in_.stimulus->apply();
            return "";
        }
        return name + ": not settable (in[N].v, out[N].load)";
    }
    return std::nullopt;
}

}  // namespace sim::sys
