#include "m150/top/served.h"

#include <span>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "helpers/hex.h"
#include "host/control.h"
#include "host/debug/methods.h"

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

}  // namespace

const sys::ServeSchema& m150_serve_schema() {
    static const sys::ServeSchema schema{"m150_sim",
                                         "m150",
                                         {{"CAN1", "can0"}, {"CAN2", ""}, {"CAN3", ""}},
                                         {"image", "symbols", "engine"}};
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

std::vector<std::string> split_paths(const std::string& list) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const std::string part = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty()) {
            out.push_back(part);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

M150Served::M150Served(M150Options options) : options_(std::move(options)) {
    e200::MachineConfig cfg;
    cfg.engine = options_.engine;
    board_ = std::make_unique<Board>(cfg);
    board_->load_flash_file(options_.image);
    sys::Symbols symbols;
    for (const std::string& path : options_.symbols) {
        symbols.merge(sys::Symbols::load_any(path));
    }
    target_ = std::make_unique<M150Target>(*board_, std::move(symbols));
    session_ = std::make_unique<sys::DebugSession>(*target_);

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
        ports_[k].owner = this;
        ports_[k].index = k;
        ports_[k].ticks_per_us = board_->m.ticks_per_us();
        board_->soc.can_bus[k].attach(ports_[k]);
    }
    if (!board_->power_on()) {
        throw std::runtime_error(options_.image + ": no reset configuration at the start of flash");
    }
}

void M150Served::log(size_t port, const sys::CanFrame& frame, uint64_t now, bool from_ecu) {
    log_.push_back({now, port, frame, from_ecu});
    logged_total_++;
    if (log_.size() > kLogLimit) {
        log_.pop_front();
    }
}

double M150Served::seconds() const {
    return target_->seconds();
}

sys::RunOutcome M150Served::run_until(double t_seconds) {
    const sys::RunResult r = session_->run_until(t_seconds);
    return sys::RunOutcome{r.reason, r.t, r.where};
}

const std::vector<std::string>& M150Served::can_ports() const {
    static const std::vector<std::string> ports{"CAN1", "CAN2", "CAN3"};
    return ports;
}

void M150Served::can_send(size_t port, const helpers::CanFrame& frame) {
    if (port < 3) {
        log(port, frame, board_->m.now(), false);
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

nlohmann::json M150Served::status() {
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
                       {"where", session_->debugger().where(m.state.pc)},
                       {"msr", m.state.msr},
                       {"waiting", m.waiting()},
                       {"engine", engine_name(m.cfg.engine)},
                       {"interrupts", m.stats().interrupts},
                       {"allow_write", options_.allow_write},
                       {"can", can}};
    if (const auto* js = m.jit_stats()) {
        out["jit"] = {{"translated", js->translated}, {"blocks_run", js->blocks_run},
                      {"interpreted", js->interpreted}, {"resets", js->resets}};
    }
    return out;
}

void M150Served::register_methods(agent_control::Dispatcher& dispatcher, const sys::ServerHooks& hooks) {
    using json = nlohmann::json;
    using sys::ControlError;
    using sys::ControlErrorReason;

    sys::DebugMethodsOptions options;
    options.app = "m150_sim";
    options.info = [this] { return info(); };
    options.status = [this] { return status(); };
    options.reset = [this] { board_->power_on(); };
    options.allow_write = options_.allow_write;
    sys::register_debug_methods(dispatcher, *session_, hooks, std::move(options));

    sys::ControlMethods d(dispatcher);
    using agent_control::MethodKind;
    d.add("status", "time, pc, MSR, the engine and its counters, the CAN controllers", [this](const json&) {
        return status();
    }, MethodKind::kReadOnly);
    d.add("console", "{lines=50}: the last lines of the console (eSCI A)", [this](const json& p) {
        const size_t n = std::min<size_t>(p.value("lines", size_t{50}), console_.size());
        json lines = json::array();
        for (size_t k = console_.size() - n; k < console_.size(); ++k) {
            lines.push_back(console_[k]);
        }
        return json{{"lines", lines}};
    }, MethodKind::kReadOnly);
    d.add("can.send", "{id, data: hex string, extended?, port=CAN1}: a frame onto a port's bus", [this](const json& p) {
        const std::string port_name = p.value("port", "CAN1");
        size_t port = 3;
        for (size_t k = 0; k < 3; ++k) {
            if (can_ports()[k] == port_name) {
                port = k;
            }
        }
        if (port == 3) {
            throw ControlError(ControlErrorReason::BadParams, "port: CAN1, CAN2 or CAN3");
        }
        sys::CanFrame f{};
        f.id = uint32_t(sys::param_unsigned(p, "id"));
        f.isExtended = p.value("extended", f.id > 0x7FF);
        if (f.id > (f.isExtended ? 0x1FFFFFFFu : 0x7FFu)) {
            throw ControlError(ControlErrorReason::BadParams, "id: does not fit the identifier");
        }
        const auto data = helpers::fromHex(sys::optional_string(p, "data").value_or(""));
        if (!data || data->size() > 8) {
            throw ControlError(ControlErrorReason::BadParams, "data: up to 8 bytes as hex");
        }
        f.len = uint8_t(data->size());
        std::copy(data->begin(), data->end(), f.data.begin());
        can_send(port, f);
        return json{{"ok", true}};
    });
    d.add("can.recv", "{since=0, limit=100, id?, port?}: logged frames (port, from_ecu, t, id, data) with their index",
          [this](const json& p) {
              const uint64_t since = sys::optional_unsigned(p, "since").value_or(0);
              const uint64_t limit = sys::optional_unsigned(p, "limit").value_or(100);
              const auto id = sys::optional_unsigned(p, "id");
              const auto port = sys::optional_string(p, "port");
              const uint64_t first = logged_total_ - log_.size();
              json frames = json::array();
              uint64_t index = first;
              for (const Logged& l : log_) {
                  if (index++ < since) {
                      continue;
                  }
                  if ((id && l.frame.id != *id) || (port && can_ports()[l.port] != *port)) {
                      continue;
                  }
                  if (frames.size() >= limit) {
                      break;
                  }
                  const uint8_t n = sys::classic_payload_bytes(l.frame);
                  frames.push_back(json{{"index", index - 1},
                                        {"t", double(l.t) / double(board_->m.cfg.core_hz)},
                                        {"port", can_ports()[l.port]},
                                        {"from_ecu", l.from_ecu},
                                        {"id", l.frame.id},
                                        {"extended", l.frame.isExtended},
                                        {"data", helpers::toHex(std::span<const uint8_t>(l.frame.data.data(), n), {},
                                                                helpers::HexCase::kUpper)}});
              }
              return json{{"next", logged_total_}, {"frames", frames}};
          }, MethodKind::kReadOnly);
}

nlohmann::json M150Served::info() const {
    return nlohmann::json{{"image", options_.image},
                          {"symbol_files", options_.symbols},
                          {"engine", engine_name(board_->m.cfg.engine)}};
}

}  // namespace sim::m150
