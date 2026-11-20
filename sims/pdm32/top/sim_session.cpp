#include "pdm32/top/sim_session.h"

#include <cmath>

#include "pdm32/top/probe.h"
#include "pdm32/top/target.h"

namespace sim::sys {

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
    debug_ = std::make_unique<DebugSession>(instance_->target());
    if (options_.scenario) {
        prepare_scenario(*instance_, *options_.scenario);
    }
}

RunResult SimSession::run_until(double t_seconds) {
    return debug_->run_until(t_seconds);
}

RunResult SimSession::run_for(double seconds) {
    return debug_->run_for(seconds);
}

RunResult SimSession::step(int count, bool over) {
    return debug_->step(count, over);
}

RunResult SimSession::finish() {
    return debug_->finish();
}

void SimSession::reset() {
    instance_->power_on();
}

void SimSession::snapshot(const std::string& name) {
    debug_->snapshot(name);
}

bool SimSession::restore(const std::string& name) {
    return debug_->restore(name);
}

json SimSession::snapshot_names() const {
    return debug_->snapshot_names();
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
                {"where", debug_->debugger().where(m.cpu.pc)},
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
