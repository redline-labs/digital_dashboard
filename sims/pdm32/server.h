// `pdm32_sim serve`: one simulated PDM32, its CAN port on zenoh and its agent methods on a control socket.
//
// The PDM's bus goes out through can_zenoh's BridgedChannel -- the same code that puts a real adapter on the bus --
// so its frames land on the topics, in the schema, with the status and .trc tap that can_bridge gives a real PDM, and
// nothing downstream can tell the difference. Frames published on the tx key reach the PDM.
//
// One thread owns the simulation: the one that calls run(). Everything that touches the device -- an agent call, a
// frame from the bus -- is handed to it and done between slices of simulated time. In real time it advances the
// simulation to the wall clock every millisecond; if it falls behind (a slow engine, a debugger stop) it says so once
// and re-anchors rather than racing to catch up, since a burst of a second's frames in a millisecond is a bus no real
// PDM produces. Stepped, time moves only when an agent calls run or step.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "agent_control/control_socket.h"
#include "agent_control/dispatcher.h"
#include "can_zenoh/bridged_channel.h"
#include "pdm32/top/sim_channel.h"
#include "pdm32/top/sim_session.h"
#include "serve_config.h"

namespace pdm32 {

struct ServeOptions {
    ServeConfig config = default_serve_config();
    bool allow_write = false;
    // Listen for agent calls here; nothing listens when empty.
    std::string control_socket;
};

class Server {
   public:
    // Builds the PDM and opens its topics. Throws std::runtime_error when it cannot (no firmware image, a scenario
    // that does not parse).
    explicit Server(ServeOptions options);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

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

   private:
    struct Task {
        std::function<void()> work;
    };

    agent_control::MethodResult invoke(const agent_control::Dispatcher::Entry& entry, const nlohmann::json& params,
                                       int timeout_ms);
    void service_tasks();
    void advance_real_time();

    ServeOptions options_;
    std::unique_ptr<sim::sys::SimSession> session_;
    std::shared_ptr<sim::sys::SimCanChannel> channel_;
    std::unique_ptr<can_zenoh::BridgedChannel> bridge_;
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

}  // namespace pdm32
