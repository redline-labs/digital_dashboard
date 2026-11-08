#include "pdm32/top/sim_session.h"

#include <cmath>

#include "hc08/disasm.h"
#include "pdm32/top/probe.h"

namespace sim::sys {

json RunResult::to_json() const {
    json j{{"reason", reason}, {"t", t}, {"pc", pc}, {"where", where}};
    if (id) {
        j["id"] = *id;
    }
    return j;
}

std::string describe_output_status(uint8_t status) {
    std::string out;
    const auto add = [&out](const char* word) { out += out.empty() ? word : std::string(", ") + word; };
    if (status & 0x01) {
        add("on");
    }
    if (status & 0x02) {
        add("fault");
    }
    if (status & 0x04) {
        add("overcurrent");
    }
    if (status & 0x08) {
        add("retries done");
    }
    return out.empty() ? "off" : out;
}

SimSession::SimSession(SessionOptions options) : options_(std::move(options)) {
    instance_ = std::make_unique<Instance>(options_.instance);
    debugger_ = std::make_unique<Debugger>(*instance_);
    if (options_.scenario) {
        prepare_scenario(*instance_, *options_.scenario);
    }
}

RunResult SimSession::result(const std::string& reason, std::optional<int> id) {
    RunResult r;
    r.reason = reason;
    r.t = instance_->seconds();
    r.pc = instance_->m().cpu.pc;
    r.where = debugger_->where(r.pc);
    r.id = id;
    return r;
}

RunResult SimSession::run_to_tick(uint64_t target) {
    Machine& m = instance_->m();
    m.clear_exit();
    for (;;) {
        const Stop stop = m.run_until(target);
        switch (stop.kind) {
            case StopKind::Watch:
                if (debugger_->on_pc(stop.pc)) {
                    m.cpu.skip_watch_pc = stop.pc;  // the next run passes it
                    return result("break", debugger_->last_breakpoint());
                }
                m.cpu.skip_watch_pc = stop.pc;
                continue;
            case StopKind::Break:
                return result("watch", debugger_->take_watch_hit());
            case StopKind::Exit:
                return result("exit");
            case StopKind::None:
            case StopKind::Limit:
            case StopKind::Wait:
            case StopKind::StopMode:
            case StopKind::Illegal:
                return result("time");
        }
    }
}

RunResult SimSession::run_until(double t_seconds) {
    return run_to_tick(instance_->ticks(t_seconds));
}

RunResult SimSession::run_for(double seconds) {
    return run_to_tick(instance_->m().now() + instance_->ticks(seconds));
}

bool SimSession::step_one() {
    Machine& m = instance_->m();
    const uint64_t cycles = m.cpu.cycles;
    m.cpu.skip_watch_pc = m.cpu.pc;
    // a sleeping CPU: time passes until an interrupt wakes it (at most a second)
    const uint64_t give_up = m.now() + m.cfg.tick_hz;
    while (m.cpu.cycles == cycles && m.now() < give_up) {
        const Stop stop = m.run_until(m.now() + 1);
        if (stop.kind == StopKind::Break || stop.kind == StopKind::Exit) {
            return false;
        }
    }
    return m.cpu.cycles != cycles;
}

RunResult SimSession::step(int count, bool over) {
    Machine& m = instance_->m();
    for (int k = 0; k < count; k++) {
        const Insn insn = decode(m.cpu.pc, [&m](uint16_t a) { return m.fetch(a); });
        if (over && (insn.op == Op::Jsr || insn.op == Op::Bsr)) {
            const uint16_t back = uint16_t(m.cpu.pc + insn.length);
            const uint16_t sp = m.cpu.sp;
            if (!step_one()) {
                return result("watch", debugger_->take_watch_hit());
            }
            // run to the return address at this stack depth
            while (!(m.cpu.pc == back && m.cpu.sp >= sp)) {
                if (!step_one()) {
                    return result("watch", debugger_->take_watch_hit());
                }
            }
            continue;
        }
        if (!step_one()) {
            return result("watch", debugger_->take_watch_hit());
        }
    }
    return result("step");
}

RunResult SimSession::finish() {
    Machine& m = instance_->m();
    const uint16_t sp = m.cpu.sp;
    for (int k = 0; k < 10'000'000; k++) {
        const Insn insn = decode(m.cpu.pc, [&m](uint16_t a) { return m.fetch(a); });
        if (!step_one()) {
            return result("watch", debugger_->take_watch_hit());
        }
        if ((insn.op == Op::Rts || insn.op == Op::Rti) && m.cpu.sp > sp) {
            return result("finish");
        }
    }
    return result("time");
}

void SimSession::reset() {
    instance_->power_on();
}

void SimSession::snapshot(const std::string& name) {
    snapshots_[name] =
        Saved{instance_->m().snapshot(), instance_->stimulus->state(), instance_->harness, instance_->can_node.signals};
}

bool SimSession::restore(const std::string& name) {
    const auto it = snapshots_.find(name);
    if (it == snapshots_.end()) {
        return false;
    }
    instance_->m().restore(it->second.machine);
    instance_->stimulus->restore(it->second.stimulus);
    instance_->harness = it->second.harness;
    instance_->can_node.signals = it->second.signals;
    return true;
}

json SimSession::snapshot_names() const {
    json out = json::array();
    for (const auto& [name, saved] : snapshots_) {
        out.push_back(name);
    }
    return out;
}

void SimSession::can_send(const CanFrame& frame) {
    instance_->can_node.send(instance_->board.can, frame, instance_->m().now());
}

json SimSession::status() {
    Machine& m = instance_->m();
    const char* mode = m.mode() == CpuMode::Run ? "run" : m.mode() == CpuMode::Wait ? "wait" : "stop";
    const uint8_t run_mode = m.mem.bytes[0x02FA];
    const char* run_modes[] = {"normal", "error (no valid config/cal)", "factory test", "sleep"};
    return json{{"t", m.seconds()},
                {"pc", m.cpu.pc},
                {"where", debugger_->where(m.cpu.pc)},
                {"cpu_mode", mode},
                {"firmware_run_mode", run_mode},
                {"firmware_run_mode_name", run_mode < 4 ? run_modes[run_mode] : "?"},
                {"cycles", m.cpu.cycles},
                {"bus_hz", m.bus_hz()},
                {"resets", m.reset_count()},
                {"engine", engine_name(m.cfg.engine)},
                {"can_frames_sent", instance_->board.soc.mscan->frames_sent()},
                {"can_frames_received", instance_->board.soc.mscan->frames_received()},
                {"battery_v", instance_->board.battery()},
                {"flash_bytes_programmed", instance_->board.soc.flash->bytes_programmed()},
                {"flash_erases", instance_->board.soc.flash->erases()},
                {"allow_write", options_.allow_write}};
}

json SimSession::outputs() {
    json out = json::array();
    Machine& m = instance_->m();
    for (size_t k = 0; k < Pdm32Board::kOutputs; k++) {
        const HiSideSwitch& sw = instance_->board.output(k);
        const uint8_t status = m.mem.bytes[0x0293 + k];
        const double load = instance_->board.load(k);
        out.push_back(json{{"output", k + 1},
                           {"pin", instance_->board.gate(k) ? 1 : 0},
                           {"v", sw.output_voltage()},
                           {"i", sw.current()},
                           {"tj", sw.junction_temperature()},
                           {"thermal_shutdown", sw.thermal_shutdown()},
                           {"load", std::isinf(load) ? json("open") : json(load)},
                           {"load_model", instance_->harness.loads[k].describe()},
                           {"status", status},
                           {"status_text", describe_output_status(status & 0x0F)}});
    }
    return out;
}

json SimSession::inputs() {
    json out = json::array();
    for (size_t k = 0; k < Pdm32Board::kInputs; k++) {
        out.push_back(json{{"input", k + 1},
                           {"v", instance_->board.input_voltage(k)},
                           {"open", instance_->board.input_open(k)},
                           {"raw", instance_->m().mem.bytes[0x01E4 + k]}});
    }
    return out;
}

}  // namespace sim::sys
