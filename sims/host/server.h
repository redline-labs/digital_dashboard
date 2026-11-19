// A simulator's `serve`: one simulated device, each of its CAN ports on zenoh, its agent methods on a control socket.
//
// Each port goes out through can_zenoh's BridgedChannel -- the same code that puts a real adapter on the bus -- so
// the device's frames land on the topics, in the schema, with the status and .trc tap that can_bridge gives a real
// one, and nothing downstream can tell the difference. Frames published on a port's tx key reach the device.
//
// One thread owns the simulation: the one that calls run(). Everything that touches the device -- an agent call, a
// frame from the bus -- is handed to it and done between slices of simulated time. In real time it advances the
// simulation to the wall clock every millisecond; if it falls behind (a slow engine, a debugger stop) it says so once
// and re-anchors rather than racing to catch up, since a burst of a second's frames in a millisecond is a bus no real
// device produces. Stepped, time moves only when an agent calls run or step.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent_control/control_socket.h"
#include "agent_control/dispatcher.h"
#include "can_zenoh/bridged_channel.h"
#include "host/serve_config.h"
#include "host/served_device.h"
#include "host/sim_channel.h"

namespace sim::sys {

struct ServeOptions {
    ServeSchema schema;
    ServeConfig config;
    // Listen for agent calls here; nothing listens when empty.
    std::string control_socket;
};

class DeviceServer {
   public:
    // Puts `device` on its topics. Throws std::runtime_error when it cannot (a socket it cannot listen on).
    DeviceServer(ServeOptions options, std::unique_ptr<ServedDevice> device);
    ~DeviceServer();
    DeviceServer(const DeviceServer&) = delete;
    DeviceServer& operator=(const DeviceServer&) = delete;

    // The simulation loop, on the calling thread, until stop() or SIGINT/SIGTERM. `ready` runs once everything is up;
    // `tick` on every pass of the loop (a health kick).
    void run(const std::function<void()>& ready = {}, const std::function<void()>& tick = {});
    // From any thread.
    void stop();

    // Runs `task` on the simulation's thread and waits for it, up to `timeout`. False if it did not run in time
    // (it still runs later). For agent calls and tests.
    bool on_sim_thread(std::function<void()> task, std::chrono::milliseconds timeout);

    agent_control::Dispatcher& dispatcher() {
        return dispatcher_;
    }
    const ServeOptions& options() const {
        return options_;
    }
    // Where the bound ports go, for the start-up log ("CAN1 on 'vehicle/can0/rx' (tx ...)").
    std::string describe_ports() const;

   private:
    struct Task {
        std::function<void()> work;
    };
    struct Port {
        size_t index;
        std::shared_ptr<SimCanChannel> channel;
        std::unique_ptr<can_zenoh::BridgedChannel> bridge;
    };

    agent_control::MethodResult invoke(const agent_control::Dispatcher::Entry& entry, const nlohmann::json& params,
                                       int timeout_ms);
    void service_tasks();
    void advance_real_time();

    ServeOptions options_;
    std::unique_ptr<ServedDevice> device_;
    std::vector<Port> ports_;
    agent_control::Dispatcher dispatcher_;
    std::unique_ptr<agent_control::ControlSocket> socket_;

    std::mutex tasks_mutex_;
    std::condition_variable tasks_changed_;
    std::deque<std::shared_ptr<Task>> tasks_;
    std::atomic<bool> stop_{false};

    // Simulation thread only.
    bool realtime_ = true;
    bool paused_ = false;
    bool behind_ = false;
    std::chrono::steady_clock::time_point wall_anchor_;
    double sim_anchor_ = 0;
};

}  // namespace sim::sys
