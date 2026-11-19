// What the serve loop (host/server.h) runs: one simulated device, whatever its CPU, as time, CAN ports and agent
// methods.
//
// The server owns the wall clock, the zenoh bridges and the control socket; the device owns everything behind its
// connector. Every call here is made on the simulation's thread, between slices of simulated time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent_control/dispatcher.h"
#include "helpers/can_frame.h"

namespace sim::sys {

// Why a run stopped: "time" (reached), "break", "watch" (a debugger stop: the server pauses), or "exit".
struct RunOutcome {
    std::string reason;
    double t = 0;
    std::string where;
};

// What the server lends the device's methods: quitting, and pausing the real-time clock.
struct ServerHooks {
    std::function<void()> quit;
    std::function<nlohmann::json(bool pause)> pause_continue;  // real time only (null in stepped mode)
    bool realtime = false;
};

class ServedDevice {
   public:
    virtual ~ServedDevice() = default;

    // Simulated time since power-on.
    virtual double seconds() const = 0;
    // Runs until `t_seconds` of simulated time, or a stop.
    virtual RunOutcome run_until(double t_seconds) = 0;

    // The device's CAN ports, by the names its serve config uses ("CAN", "CAN1"), in a fixed order.
    virtual const std::vector<std::string>& can_ports() const = 0;
    // A frame from the outside world onto port `port`, now.
    virtual void can_send(size_t port, const helpers::CanFrame& frame) = 0;
    // Where the frames the device puts on port `port` go: `sink(frame, microseconds since power-on)`. Null: nowhere.
    virtual void set_can_sink(size_t port, std::function<void(const helpers::CanFrame&, uint64_t sim_us)> sink) = 0;
    // The bit rate the device's controller on `port` is programmed to (0 until it is).
    virtual uint32_t can_bitrate(size_t port) const = 0;

    // The device's agent methods (the server adds app.logs and app.info).
    virtual void register_methods(agent_control::Dispatcher& dispatcher, const ServerHooks& hooks) = 0;
    // What it is running, for app.info (firmware, scenario, ...).
    virtual nlohmann::json info() const = 0;
};

}  // namespace sim::sys
