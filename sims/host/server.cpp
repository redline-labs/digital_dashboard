#include "host/server.h"

#include <unistd.h>

#include <future>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

#include "agent_control/log_ring.h"
#include "cli/interrupt.h"

namespace sim::sys {

namespace {

// How far the simulation may trail the wall clock before it stops trying to catch up.
constexpr double kMaxLagSeconds = 0.25;
// How often the loop wakes when nothing asks it to.
constexpr std::chrono::milliseconds kTick{1};

}  // namespace

DeviceServer::DeviceServer(ServeOptions options, std::unique_ptr<ServedDevice> device)
    : options_(std::move(options)),
      device_(std::move(device)),
      dispatcher_(options_.schema.app, [this](const std::string&, const agent_control::Dispatcher::Entry& entry,
                                              const nlohmann::json& params,
                                              int timeout_ms) { return invoke(entry, params, timeout_ms); }) {
    realtime_ = !options_.config.stepped;

    const std::vector<std::string>& names = device_->can_ports();
    if (names.size() != options_.config.can.size()) {
        throw std::runtime_error("serve: the config binds " + std::to_string(options_.config.can.size()) +
                                 " CAN ports, the device has " + std::to_string(names.size()));
    }
    for (size_t k = 0; k < names.size(); k++) {
        const CanBinding& binding = options_.config.can[k];
        if (!binding.enabled) {
            continue;
        }
        Port port;
        port.index = k;
        port.channel = std::make_shared<SimCanChannel>(binding.bridge.device);
        port.channel->set_device_bitrate(device_->can_bitrate(k));
        device_->set_can_sink(k, [channel = port.channel](const helpers::CanFrame& frame, uint64_t sim_us) {
            channel->deliver(frame, sim_us);
        });
        ports_.push_back(std::move(port));
    }

    ServerHooks hooks;
    hooks.realtime = realtime_;
    hooks.quit = [this]() { stop(); };
    if (realtime_) {
        hooks.pause_continue = [this](bool pause) {
            paused_ = pause;
            behind_ = false;
            wall_anchor_ = std::chrono::steady_clock::now();
            sim_anchor_ = device_->seconds();
            return nlohmann::json{{"paused", paused_}, {"t", sim_anchor_}};
        };
    }
    device_->register_methods(dispatcher_, hooks);
    dispatcher_.registerMethod("app.logs", agent_control::queryLogs, agent_control::MethodKind::kReadOnly,
                               "{since_seq?, level?, grep?, logger?, limit?}: this process's log");
    dispatcher_.registerMethod(
        "app.info",
        [this](const nlohmann::json&) -> agent_control::MethodResult {
            nlohmann::json info = device_->info();
            info["app"] = options_.schema.app;
            info["pid"] = ::getpid();
            info["time"] = realtime_ ? "real" : "stepped";
            nlohmann::json can = nlohmann::json::object();
            for (const Port& port : ports_) {
                const can_zenoh::BridgeConfig& bridge = options_.config.can[port.index].bridge;
                can[device_->can_ports()[port.index]] = {{"rx_key", bridge.rxKey}, {"tx_key", bridge.txKey}};
            }
            info["can"] = can;
            if (device_->can_ports().size() == 1 && ports_.size() == 1) {  // a one-port device's bus, at the top
                info["rx_key"] = options_.config.can[0].bridge.rxKey;
                info["tx_key"] = options_.config.can[0].bridge.txKey;
            }
            return info;
        },
        agent_control::MethodKind::kReadOnly, "what this simulator is running and where its buses go");

    for (Port& port : ports_) {
        port.bridge = std::make_unique<can_zenoh::BridgedChannel>(options_.config.can[port.index].bridge, port.channel);
        port.bridge->start();
    }

    if (!options_.control_socket.empty()) {
        socket_ = std::make_unique<agent_control::ControlSocket>(
            [this](std::string_view line) { return dispatcher_.handleLine(line); });
        if (!socket_->start(options_.control_socket)) {
            throw std::runtime_error("cannot listen on '" + options_.control_socket + "'");
        }
    }
}

DeviceServer::~DeviceServer() {
    // Callers first, then the bridges' threads, then the device they reach into.
    socket_.reset();
    for (Port& port : ports_) {
        if (port.bridge) {
            port.bridge->stop();
        }
        port.channel->stop();
        device_->set_can_sink(port.index, nullptr);
    }
}

std::string DeviceServer::describe_ports() const {
    std::string out;
    for (const Port& port : ports_) {
        const can_zenoh::BridgeConfig& bridge = options_.config.can[port.index].bridge;
        out += (out.empty() ? "" : ", ") + device_->can_ports()[port.index] + " on '" + bridge.rxKey + "' (tx '" +
               bridge.txKey + "')";
    }
    return out.empty() ? "no CAN port on the bus" : out;
}

void DeviceServer::stop() {
    stop_ = true;
    tasks_changed_.notify_all();
}

bool DeviceServer::on_sim_thread(std::function<void()> task, std::chrono::milliseconds timeout) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    auto entry = std::make_shared<Task>();
    entry->work = [work = std::move(task), done]() {
        work();
        done->set_value();
    };
    {
        const std::lock_guard<std::mutex> lock(tasks_mutex_);
        tasks_.push_back(std::move(entry));
    }
    tasks_changed_.notify_all();
    return finished.wait_for(timeout) == std::future_status::ready;
}

agent_control::MethodResult DeviceServer::invoke(const agent_control::Dispatcher::Entry& entry,
                                                 const nlohmann::json& params, int timeout_ms) {
    // Held by shared_ptr: after a timeout the task may still run, and must not write into this frame.
    auto result = std::make_shared<agent_control::MethodResult>(std::unexpected(agent_control::internalError("")));
    const bool ran = on_sim_thread(
        [entry, params, result]() { *result = agent_control::Dispatcher::runHandler(entry, params); },
        std::chrono::milliseconds(timeout_ms));
    if (!ran) {
        return std::unexpected(agent_control::AgentError{
            agent_control::ErrorCode::kTimeout,
            "The simulator did not answer within " + std::to_string(timeout_ms) +
                " ms; a long run may still be in progress. Pass a larger _timeout_ms.",
            nlohmann::json{{"timeout_ms", timeout_ms}}});
    }
    return *result;
}

void DeviceServer::service_tasks() {
    std::deque<std::shared_ptr<Task>> ready;
    {
        const std::lock_guard<std::mutex> lock(tasks_mutex_);
        ready.swap(tasks_);
    }
    for (const auto& task : ready) {
        task->work();
    }
}

void DeviceServer::advance_real_time() {
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_anchor_).count();
    const double target = sim_anchor_ + wall;
    const double now = device_->seconds();
    if (target - now > kMaxLagSeconds) {
        if (!behind_) {
            SPDLOG_WARN("[sim] {:.3f} s behind the wall clock; carrying on from here rather than catching up",
                        target - now);
            behind_ = true;
        }
        wall_anchor_ = std::chrono::steady_clock::now();
        sim_anchor_ = now;
        return;
    }
    const RunOutcome stop = device_->run_until(target);
    if (stop.reason == "break" || stop.reason == "watch") {
        paused_ = true;
        SPDLOG_INFO("[sim] stopped at a {} at t={:.6f} s ({}); paused until continue", stop.reason, stop.t,
                    stop.where);
    }
}

void DeviceServer::run(const std::function<void()>& ready, const std::function<void()>& tick) {
    wall_anchor_ = std::chrono::steady_clock::now();
    sim_anchor_ = device_->seconds();
    if (ready) {
        ready();
    }

    while (!stop_ && !cli::interrupted()) {
        service_tasks();
        for (Port& port : ports_) {
            for (const helpers::CanFrame& frame : port.channel->take_outbound()) {
                device_->can_send(port.index, frame);
            }
        }
        if (realtime_ && !paused_) {
            advance_real_time();
        }
        for (Port& port : ports_) {
            port.channel->set_device_bitrate(device_->can_bitrate(port.index));
        }
        if (tick) {
            tick();
        }

        std::unique_lock<std::mutex> lock(tasks_mutex_);
        tasks_changed_.wait_for(lock, kTick, [this] { return stop_ || !tasks_.empty(); });
    }
    // Whatever was asked for in the meantime still gets an answer.
    service_tasks();
}

}  // namespace sim::sys
