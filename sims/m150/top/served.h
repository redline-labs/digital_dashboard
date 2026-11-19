// The simulated M150 as host/server.h serves it: the board as a ServedDevice, and what `m150_sim serve`'s config may
// say: three CAN ports, CAN1 (FlexCAN A) on can0 by default and CAN2/CAN3 (FlexCAN B, C) off until named; the image
// and the engine.
//
// Which FlexCAN drives which connector pair is not known; A, B, C are taken as CAN1, CAN2, CAN3.
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "host/serve_config.h"
#include "host/served_device.h"
#include "m150/board/m150.h"
#include "m150/top/candump.h"

namespace sim::m150 {

const sys::ServeSchema& m150_serve_schema();

// "interp"/"interpreter", "block" or "jit"; nullopt otherwise.
std::optional<e200::Engine> parse_engine(const std::string& name);

class M150Served final : public sys::ServedDevice {
   public:
    // Boots `image`. Throws std::runtime_error when it cannot be read or holds no reset configuration.
    M150Served(const std::string& image, e200::Engine engine);

    Board& board() {
        return *board_;
    }

    double seconds() const override;
    sys::RunOutcome run_until(double t_seconds) override;
    const std::vector<std::string>& can_ports() const override;
    void can_send(size_t port, const helpers::CanFrame& frame) override;
    void set_can_sink(size_t port, std::function<void(const helpers::CanFrame&, uint64_t sim_us)> sink) override;
    uint32_t can_bitrate(size_t port) const override;
    void register_methods(agent_control::Dispatcher& dispatcher, const sys::ServerHooks& hooks) override;
    nlohmann::json info() const override;

    // the console's last lines (eSCI A)
    const std::deque<std::string>& console() const {
        return console_;
    }

   private:
    // The outside world on one bus: frames from it are sent from here, and every frame the board puts on the bus
    // reaches the sink.
    class Port final : public sys::CanNode {
       public:
        std::function<void(const helpers::CanFrame&, uint64_t sim_us)> sink;
        uint64_t ticks_per_us = 125;
        bool receive(const sys::CanFrame& frame, const sys::CanNode&, uint64_t now) override {
            if (sink) {
                sink(frame, now / ticks_per_us);
            }
            return true;
        }
    };

    nlohmann::json status() const;

    std::string image_;
    std::unique_ptr<Board> board_;
    std::array<Port, 3> ports_;
    std::deque<std::string> console_;
    std::string line_;
};

}  // namespace sim::m150
