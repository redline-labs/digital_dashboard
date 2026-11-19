// The simulated PDM as host/server.h serves it: its session as a ServedDevice, and what `pdm32_sim serve`'s config
// may say (one CAN port, "CAN", on can0 by default; scenario, firmware and engine).
#pragma once

#include <memory>

#include "host/serve_config.h"
#include "host/served_device.h"
#include "pdm32/top/sim_session.h"

namespace sim::sys {

const ServeSchema& pdm32_serve_schema();

// The session a serve config describes. Throws std::runtime_error when it cannot be built (no firmware image, a
// scenario that does not parse, an unknown engine).
SessionOptions pdm32_session_options(const ServeConfig& config, bool allow_write);

class Pdm32Served final : public ServedDevice {
   public:
    explicit Pdm32Served(SessionOptions options);

    SimSession& session() {
        return session_;
    }

    double seconds() const override;
    RunOutcome run_until(double t_seconds) override;
    const std::vector<std::string>& can_ports() const override;
    void can_send(size_t port, const helpers::CanFrame& frame) override;
    void set_can_sink(size_t port, std::function<void(const helpers::CanFrame&, uint64_t sim_us)> sink) override;
    uint32_t can_bitrate(size_t port) const override;
    void register_methods(agent_control::Dispatcher& dispatcher, const ServerHooks& hooks) override;
    nlohmann::json info() const override;

   private:
    mutable SimSession session_;
    std::string scenario_path_;
};

}  // namespace sim::sys
