#include "m150/top/target.h"

#include <array>
#include <cstring>
#include <regex>

#include "e200/isa.h"

namespace sim::m150 {

namespace {

using e200::CpuState;

// The registers by name: where each lives in the CpuState.
struct Named {
    const char* name;
    uint32_t CpuState::*field;
};
constexpr Named kSpecial[] = {
    {"pc", &CpuState::pc},       {"lr", &CpuState::lr},       {"ctr", &CpuState::ctr},     {"cr", &CpuState::cr},
    {"xer", &CpuState::xer},     {"msr", &CpuState::msr},     {"srr0", &CpuState::srr0},   {"srr1", &CpuState::srr1},
    {"csrr0", &CpuState::csrr0}, {"csrr1", &CpuState::csrr1}, {"dear", &CpuState::dear},   {"esr", &CpuState::esr},
    {"ivpr", &CpuState::ivpr},   {"pid0", &CpuState::pid0},   {"spefscr", &CpuState::spefscr},
    {"tcr", &CpuState::tcr},     {"tsr", &CpuState::tsr},     {"hid0", &CpuState::hid0},
};

const std::regex kGpr(R"(^r([0-9]|[12][0-9]|3[01])$)");
const std::regex kPort(R"(^can([123])\.(bit_rate|sent|received)$)");

struct Saved {
    e200::Machine::Snapshot machine;
};

}  // namespace

double M150Target::seconds() const {
    return double(b_.m.now()) / double(b_.m.cfg.core_hz);
}
uint64_t M150Target::now() const {
    return b_.m.now();
}
uint64_t M150Target::ticks(double s) const {
    return uint64_t(s * double(b_.m.cfg.core_hz) + 0.5);
}

uint64_t M150Target::pc() const {
    return b_.m.state.pc;
}
uint64_t M150Target::stack_pointer() const {
    return b_.m.state.gpr[1];
}

nlohmann::json M150Target::registers() const {
    const CpuState& s = b_.m.state;
    nlohmann::json out = nlohmann::json::object();
    for (int r = 0; r < 32; ++r) {
        out["r" + std::to_string(r)] = s.gpr[size_t(r)];
    }
    for (const Named& n : kSpecial) {
        out[n.name] = s.*n.field;
    }
    out["dec"] = s.decrementer();
    out["tb"] = s.timebase();
    out["cycles"] = s.cycles;
    return out;
}

std::optional<uint64_t> M150Target::read_register(const std::string& name) const {
    const CpuState& s = b_.m.state;
    std::smatch m;
    if (std::regex_match(name, m, kGpr)) {
        return s.gpr[size_t(std::stoi(m[1].str()))];
    }
    for (const Named& n : kSpecial) {
        if (name == n.name) {
            return s.*n.field;
        }
    }
    if (name == "sp") {
        return s.gpr[1];
    }
    if (name == "dec") {
        return s.decrementer();
    }
    if (name == "tb") {
        return s.timebase();
    }
    return std::nullopt;
}

bool M150Target::write_register(const std::string& name, uint64_t value) {
    CpuState& s = b_.m.state;
    const uint32_t v = uint32_t(value);
    std::smatch m;
    if (std::regex_match(name, m, kGpr)) {
        s.gpr[size_t(std::stoi(m[1].str()))] = v;
        return true;
    }
    if (name == "sp") {
        s.gpr[1] = v;
        return true;
    }
    for (const Named& n : kSpecial) {
        if (name == n.name) {
            s.*n.field = name == "pc" ? (v & ~3u) : v;
            if (name == "msr" || name == "pid0") {
                b_.m.translation_changed();
            }
            return true;
        }
    }
    return false;
}

bool M150Target::peek(uint64_t address, uint8_t* out, size_t size) {
    for (size_t k = 0; k < size; ++k) {
        uint32_t v = 0;
        if (address + k > 0xFFFFFFFFull || !b_.m.peek(uint32_t(address + k), 1, &v)) {
            return false;
        }
        out[k] = uint8_t(v);
    }
    return true;
}

bool M150Target::poke(uint64_t address, const uint8_t* in, size_t size) {
    for (size_t k = 0; k < size; ++k) {
        if (address + k > 0xFFFFFFFFull || !b_.m.poke(uint32_t(address + k), 1, in[k])) {
            return false;
        }
    }
    return true;
}

std::optional<uint32_t> M150Target::word(uint32_t ea) {
    uint32_t v = 0;
    return b_.m.peek(ea, 4, &v) ? std::optional<uint32_t>(v) : std::nullopt;
}

sys::Decoded M150Target::decode(uint64_t address) {
    sys::Decoded d;
    d.length = 4;
    const auto w = word(uint32_t(address));
    if (!w) {
        d.text = "??";
        d.valid = false;
        return d;
    }
    const e200::Insn in = e200::decode(*w);
    d.text = e200::disassemble(in, uint32_t(address));
    const bool branch = in.op == e200::Op::B || in.op == e200::Op::Bc || in.op == e200::Op::Bclr ||
                        in.op == e200::Op::Bcctr;
    d.call = branch && in.has(e200::flag::kLk);
    d.ret = in.op == e200::Op::Bclr && !in.has(e200::flag::kLk);
    return d;
}

void M150Target::watch_pc(uint64_t pc, bool on) {
    b_.m.watch_pc(uint32_t(pc), on);
}
void M150Target::skip_pc_once(uint64_t pc) {
    b_.m.state.skip_watch_pc = int64_t(pc);
}

int M150Target::add_data_watch(uint64_t begin, uint64_t end, bool on_read, bool on_write, DataWatchFn fn) {
    e200::Machine::DataWatch w;
    w.begin = uint32_t(begin);
    w.end = uint32_t(std::min<uint64_t>(end, 0xFFFFFFFFull));
    w.on_read = on_read;
    w.on_write = on_write;
    w.fn = [callback = std::move(fn)](uint32_t a, uint8_t value, bool write) { return callback(a, value, write); };
    return b_.m.add_data_watch(std::move(w));
}
void M150Target::remove_data_watch(int id) {
    b_.m.remove_data_watch(id);
}

namespace {
sys::TargetStop from(const e200::Stop& stop) {
    switch (stop.kind) {
        case e200::StopKind::Watch:
            return {sys::TargetStopKind::PcWatch, stop.pc};
        case e200::StopKind::Break:
            return {sys::TargetStopKind::DataWatch, stop.pc};
        case e200::StopKind::Exit:
            return {sys::TargetStopKind::Exit, stop.pc};
        case e200::StopKind::Checkstop:
            return {sys::TargetStopKind::Fault, stop.pc};
        case e200::StopKind::None:
        case e200::StopKind::Limit:
        case e200::StopKind::Wait:
            break;
    }
    return {sys::TargetStopKind::Time, stop.pc};
}
}  // namespace

sys::TargetStop M150Target::run_until(uint64_t tick) {
    return from(b_.m.run_until(tick));
}

sys::TargetStop M150Target::step_instruction(uint64_t max_ticks) {
    bool executed = false;
    const sys::TargetStop stop = from(b_.m.step_instruction(max_ticks, &executed));
    stepped_ = executed;
    return stop.kind == sys::TargetStopKind::PcWatch ? sys::TargetStop{sys::TargetStopKind::Time, stop.pc} : stop;
}

void M150Target::clear_exit() {
    b_.m.clear_exit();
}

void M150Target::reset() {
    b_.power_on();
}

bool M150Target::after_call(uint32_t ret) {
    // the instruction before a return address is a branch that set LR
    const auto w = word(ret - 4);
    if (!w) {
        return false;
    }
    const e200::Insn in = e200::decode(*w);
    const bool branch = in.op == e200::Op::B || in.op == e200::Op::Bc || in.op == e200::Op::Bclr ||
                        in.op == e200::Op::Bcctr;
    return branch && in.has(e200::flag::kLk);
}

std::vector<uint64_t> M150Target::return_addresses() {
    // LR (the innermost return, until the function saves it), then the LR save words up the back chain: each frame's
    // first word is the caller's stack pointer, and a function saves LR in its caller's frame at +4.
    std::vector<uint64_t> out;
    const CpuState& s = b_.m.state;
    if (s.lr != 0 && after_call(s.lr)) {
        out.push_back(s.lr);
    }
    uint32_t sp = s.gpr[1];
    for (int depth = 0; depth < 32 && out.size() < 16; ++depth) {
        const auto next = word(sp);
        if (!next || *next == 0 || *next <= sp) {
            break;
        }
        const auto ret = word(*next + 4);
        if (ret && after_call(*ret) && (out.empty() || out.back() != *ret)) {
            out.push_back(*ret);
        }
        sp = *next;
    }
    return out;
}

std::any M150Target::snapshot() {
    return Saved{b_.m.snapshot()};
}

void M150Target::restore(const std::any& snapshot) {
    b_.m.restore(std::any_cast<const Saved&>(snapshot).machine);
}

std::optional<double> M150Target::read_device_name(const std::string& name, std::string& error) {
    (void)error;
    std::smatch m;
    if (std::regex_match(name, m, kPort)) {
        const auto& can = *b_.soc.can[size_t(std::stoi(m[1].str()) - 1)];
        const std::string field = m[2];
        if (field == "bit_rate") {
            return can.bit_rate();
        }
        return double(field == "sent" ? can.frames_sent() : can.frames_received());
    }
    if (name == "interrupts") {
        return double(b_.m.stats().interrupts);
    }
    if (name == "waiting") {
        return b_.m.waiting() ? 1.0 : 0.0;
    }
    return std::nullopt;
}

}  // namespace sim::m150
