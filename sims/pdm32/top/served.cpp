#include "pdm32/top/served.h"

#include <stdexcept>
#include <utility>

#include "pdm32/top/methods.h"
#include "pdm32/top/scenario.h"

namespace sim::sys {

const ServeSchema& pdm32_serve_schema() {
    static const ServeSchema schema{"pdm32_sim", "pdm32", {{"CAN", "can0"}}, {"scenario", "firmware", "engine"}};
    return schema;
}

SessionOptions pdm32_session_options(const ServeConfig& config, bool allow_write) {
    const std::string scenario_path = config.device_value("scenario");
    const std::string firmware = config.device_value("firmware");
    SessionOptions options;
    if (!scenario_path.empty()) {
        options.scenario = load_scenario(scenario_path);
        options.instance = instance_config_for(*options.scenario, firmware);
    } else {
        Scenario empty;
        empty.path = "<none>";
        options.instance = instance_config_for(empty, firmware);
    }
    if (const std::string engine = config.device_value("engine"); !engine.empty()) {
        const auto parsed = parse_engine(engine);
        if (!parsed) {
            throw std::runtime_error("engine: '" + engine + "' is not interp, block or jit");
        }
        options.instance.engine = *parsed;
    }
    options.allow_write = allow_write;
    return options;
}

Pdm32Served::Pdm32Served(SessionOptions options)
    : session_(options), scenario_path_(options.scenario ? options.scenario->path : std::string()) {
}

double Pdm32Served::seconds() const {
    return session_.instance().seconds();
}

RunOutcome Pdm32Served::run_until(double t_seconds) {
    const RunResult r = session_.run_until(t_seconds);
    return RunOutcome{r.reason, r.t, r.where};
}

const std::vector<std::string>& Pdm32Served::can_ports() const {
    static const std::vector<std::string> ports{"CAN"};
    return ports;
}

void Pdm32Served::can_send(size_t port, const helpers::CanFrame& frame) {
    (void)port;
    session_.can_send(frame);
}

void Pdm32Served::set_can_sink(size_t port, std::function<void(const helpers::CanFrame&, uint64_t sim_us)> sink) {
    (void)port;
    Instance& in = session_.instance();
    if (!sink) {
        in.can_node.on_frame = nullptr;
        return;
    }
    in.can_node.on_frame = [&in, deliver = std::move(sink)](const helpers::CanFrame& frame, uint64_t tick) {
        deliver(frame, tick / in.m().ticks_per_us());
    };
}

uint32_t Pdm32Served::can_bitrate(size_t port) const {
    (void)port;
    return session_.instance().board.soc.mscan->bit_rate();
}

void Pdm32Served::register_methods(agent_control::Dispatcher& dispatcher, const ServerHooks& hooks) {
    sim::sys::register_methods(dispatcher, session_, hooks);
}

nlohmann::json Pdm32Served::info() const {
    return nlohmann::json{{"scenario", scenario_path_}, {"firmware", session_.instance().cfg.firmware_path}};
}

}  // namespace sim::sys
