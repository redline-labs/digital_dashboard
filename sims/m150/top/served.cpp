#include "m150/top/served.h"

#include <stdexcept>

#include <spdlog/spdlog.h>

#include "host/control.h"

namespace sim::m150 {

namespace {
constexpr size_t kConsoleLines = 200;

const char* engine_name(e200::Engine e) {
    switch (e) {
        case e200::Engine::Interpreter: return "interp";
        case e200::Engine::Block: return "block";
        case e200::Engine::Jit: return "jit";
    }
    return "?";
}

const char* stop_reason(e200::StopKind kind) {
    switch (kind) {
        case e200::StopKind::Limit:
        case e200::StopKind::None:
        case e200::StopKind::Wait:
            return "time";
        case e200::StopKind::Watch:
            return "watch";
        case e200::StopKind::Break:
        case e200::StopKind::Checkstop:
            return "break";
        case e200::StopKind::Exit:
            return "exit";
    }
    return "time";
}
}  // namespace

const sys::ServeSchema& m150_serve_schema() {
    static const sys::ServeSchema schema{
        "m150_sim", "m150", {{"CAN1", "can0"}, {"CAN2", ""}, {"CAN3", ""}}, {"image", "engine"}};
    return schema;
}

std::optional<e200::Engine> parse_engine(const std::string& name) {
    if (name == "interp" || name == "interpreter") {
        return e200::Engine::Interpreter;
    }
    if (name == "block") {
        return e200::Engine::Block;
    }
    if (name == "jit") {
        return e200::Engine::Jit;
    }
    return std::nullopt;
}

M150Served::M150Served(const std::string& image, e200::Engine engine) : image_(image) {
    e200::MachineConfig cfg;
    cfg.engine = engine;
    board_ = std::make_unique<Board>(cfg);
    board_->load_flash_file(image);
    board_->soc.esci_a->on_byte = [this](uint8_t b) {
        if (b == '\n') {
            SPDLOG_INFO("[m150] {}", line_);
            console_.push_back(std::move(line_));
            line_.clear();
            if (console_.size() > kConsoleLines) {
                console_.pop_front();
            }
        } else if (b != '\r') {
            line_ += char(b);
        }
    };
    for (size_t k = 0; k < 3; ++k) {
        ports_[k].ticks_per_us = board_->m.ticks_per_us();
        board_->soc.can_bus[k].attach(ports_[k]);
    }
    if (!board_->reset()) {
        throw std::runtime_error(image + ": no reset configuration at the start of flash");
    }
}

double M150Served::seconds() const {
    return double(board_->m.now()) / double(board_->m.cfg.core_hz);
}

sys::RunOutcome M150Served::run_until(double t_seconds) {
    const e200::Stop stop = board_->m.run_until(uint64_t(t_seconds * double(board_->m.cfg.core_hz)));
    char where[32];
    std::snprintf(where, sizeof where, "0x%08x", stop.pc);
    return sys::RunOutcome{stop_reason(stop.kind), seconds(), where};
}

const std::vector<std::string>& M150Served::can_ports() const {
    static const std::vector<std::string> ports{"CAN1", "CAN2", "CAN3"};
    return ports;
}

void M150Served::can_send(size_t port, const helpers::CanFrame& frame) {
    if (port < 3) {
        board_->soc.can_bus[port].send(ports_[port], frame, board_->m.now());
    }
}

void M150Served::set_can_sink(size_t port, std::function<void(const helpers::CanFrame&, uint64_t sim_us)> sink) {
    if (port < 3) {
        ports_[port].sink = std::move(sink);
    }
}

uint32_t M150Served::can_bitrate(size_t port) const {
    return port < 3 ? board_->soc.can[port]->bit_rate() : 0;
}

nlohmann::json M150Served::status() const {
    const e200::Machine& m = board_->m;
    nlohmann::json can = nlohmann::json::array();
    for (size_t k = 0; k < 3; ++k) {
        can.push_back({{"port", can_ports()[k]},
                       {"bit_rate", board_->soc.can[k]->bit_rate()},
                       {"sent", board_->soc.can[k]->frames_sent()},
                       {"received", board_->soc.can[k]->frames_received()}});
    }
    nlohmann::json out{{"t", seconds()},
                       {"pc", m.state.pc},
                       {"msr", m.state.msr},
                       {"waiting", m.waiting()},
                       {"engine", engine_name(m.cfg.engine)},
                       {"interrupts", m.stats().interrupts},
                       {"can", can}};
    if (const auto* js = m.jit_stats()) {
        out["jit"] = {{"translated", js->translated}, {"blocks_run", js->blocks_run},
                      {"interpreted", js->interpreted}, {"resets", js->resets}};
    }
    return out;
}

void M150Served::register_methods(agent_control::Dispatcher& dispatcher, const sys::ServerHooks& hooks) {
    sys::ControlMethods d(dispatcher);
    using agent_control::MethodKind;
    d.add("status", "time, pc, MSR, the engine and its counters, the CAN controllers", [this](const nlohmann::json&) {
        return status();
    }, MethodKind::kReadOnly);
    d.add("console", "{lines=50}: the last lines of the console (eSCI A)", [this](const nlohmann::json& p) {
        const size_t n = std::min<size_t>(p.value("lines", size_t{50}), console_.size());
        nlohmann::json lines = nlohmann::json::array();
        for (size_t k = console_.size() - n; k < console_.size(); ++k) {
            lines.push_back(console_[k]);
        }
        return nlohmann::json{{"lines", lines}};
    }, MethodKind::kReadOnly);
    d.add("run", "advance time by {seconds} (stepped mode)", [this, realtime = hooks.realtime](const nlohmann::json& p) {
        if (realtime) {
            throw sys::ControlError(sys::ControlErrorReason::WrongMode, "run: realtime mode (serve --stepped)");
        }
        const sys::RunOutcome r = run_until(seconds() + sys::param_real(p, "seconds"));
        return nlohmann::json{{"reason", r.reason}, {"t", r.t}, {"where", r.where}};
    });
    if (hooks.quit) {
        d.add("quit", "stop the simulator", [quit = hooks.quit](const nlohmann::json&) {
            quit();
            return nlohmann::json{{"ok", true}};
        });
    }
}

nlohmann::json M150Served::info() const {
    return nlohmann::json{{"image", image_}, {"engine", engine_name(board_->m.cfg.engine)}};
}

}  // namespace sim::m150
