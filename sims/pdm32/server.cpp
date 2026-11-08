#include "server.h"

#include <unistd.h>

#include <future>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

#include "agent_control/log_ring.h"
#include "cli/interrupt.h"
#include "pdm32/top/methods.h"
#include "pdm32/top/scenario.h"

namespace pdm32 {

namespace {

using namespace sim;
using namespace sim::sys;

// How far the simulation may trail the wall clock before it stops trying to catch up.
constexpr double kMaxLagSeconds = 0.25;
// How often the loop wakes when nothing asks it to.
constexpr std::chrono::milliseconds kTick{1};

SessionOptions session_options(const ServeOptions& options) {
    const ServeConfig& config = options.config;
    SessionOptions so;
    if (!config.scenario.empty()) {
        so.scenario = load_scenario(config.scenario);
        so.instance = instance_config_for(*so.scenario, config.firmware);
    } else {
        Scenario empty;
        empty.path = "<none>";
        so.instance = instance_config_for(empty, config.firmware);
    }
    if (!config.engine.empty()) {
        const auto engine = parse_engine(config.engine);
        if (!engine) {
            throw std::runtime_error("engine: '" + config.engine + "' is not interp, block or jit");
        }
        so.instance.engine = *engine;
    }
    so.allow_write = options.allow_write;
    return so;
}

}  // namespace

Server::Server(ServeOptions options)
    : options_(std::move(options)),
      session_(std::make_unique<SimSession>(session_options(options_))),
      channel_(std::make_shared<SimCanChannel>(options_.config.can.device)),
      dispatcher_("pdm32_sim", [this](const std::string&, const agent_control::Dispatcher::Entry& entry,
                                      const nlohmann::json& params,
                                      int timeout_ms) { return invoke(entry, params, timeout_ms); }) {
    realtime_ = !options_.config.stepped;

    Instance& in = session_->instance();
    in.can_node.on_frame = [this, &in](const helpers::CanFrame& frame, uint64_t tick) {
        channel_->deliver(frame, tick / in.m().ticks_per_us());
    };
    channel_->set_device_bitrate(in.board.soc.mscan->bit_rate());

    ServerHooks hooks;
    hooks.realtime = realtime_;
    hooks.quit = [this]() { stop(); };
    if (realtime_) {
        hooks.pause_continue = [this](bool pause) {
            paused_ = pause;
            behind_ = false;
            wall_anchor_ = std::chrono::steady_clock::now();
            sim_anchor_ = session_->instance().seconds();
            return json{{"paused", paused_}, {"t", sim_anchor_}};
        };
    }
    register_methods(dispatcher_, *session_, hooks);
    dispatcher_.registerMethod("app.logs", agent_control::queryLogs, agent_control::MethodKind::kReadOnly,
                               "{since_seq?, level?, grep?, logger?, limit?}: this process's log");
    dispatcher_.registerMethod(
        "app.info",
        [this](const json&) -> agent_control::MethodResult {
            const ServeConfig& config = options_.config;
            return json{{"app", "pdm32_sim"},
                        {"pid", ::getpid()},
                        {"time", realtime_ ? "real" : "stepped"},
                        {"rx_key", config.can.rxKey},
                        {"tx_key", config.can.txKey},
                        {"scenario", config.scenario},
                        {"firmware", session_->instance().cfg.firmware_path}};
        },
        agent_control::MethodKind::kReadOnly, "what this simulator is running and where its bus goes");

    bridge_ = std::make_unique<can_zenoh::BridgedChannel>(options_.config.can, channel_);
    bridge_->start();

    if (!options_.control_socket.empty()) {
        socket_ = std::make_unique<agent_control::ControlSocket>(
            [this](std::string_view line) { return dispatcher_.handleLine(line); });
        if (!socket_->start(options_.control_socket)) {
            throw std::runtime_error("cannot listen on '" + options_.control_socket + "'");
        }
    }
}

Server::~Server() {
    // Callers first, then the bridge's threads, then the device they reach into.
    socket_.reset();
    if (bridge_) {
        bridge_->stop();
    }
    channel_->stop();
    session_->instance().can_node.on_frame = nullptr;
}

void Server::stop() {
    stop_ = true;
    tasks_changed_.notify_all();
}

bool Server::on_sim_thread(std::function<void()> task, std::chrono::milliseconds timeout) {
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

agent_control::MethodResult Server::invoke(const agent_control::Dispatcher::Entry& entry, const nlohmann::json& params,
                                           int timeout_ms) {
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

void Server::service_tasks() {
    std::deque<std::shared_ptr<Task>> ready;
    {
        const std::lock_guard<std::mutex> lock(tasks_mutex_);
        ready.swap(tasks_);
    }
    for (const auto& task : ready) {
        task->work();
    }
}

void Server::advance_real_time() {
    Instance& in = session_->instance();
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_anchor_).count();
    const double target = sim_anchor_ + wall;
    const double now = in.seconds();
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
    const RunResult stop = session_->run_until(target);
    if (stop.reason == "break" || stop.reason == "watch") {
        paused_ = true;
        SPDLOG_INFO("[sim] stopped at a {} at t={:.6f} s ({}); paused until continue", stop.reason, stop.t,
                    stop.where);
    }
}

void Server::run(const std::function<void()>& ready, const std::function<void()>& tick) {
    wall_anchor_ = std::chrono::steady_clock::now();
    sim_anchor_ = session_->instance().seconds();
    if (ready) {
        ready();
    }

    Instance& in = session_->instance();
    while (!stop_ && !cli::interrupted()) {
        service_tasks();
        for (const helpers::CanFrame& frame : channel_->take_outbound()) {
            session_->can_send(frame);
        }
        if (realtime_ && !paused_) {
            advance_real_time();
        }
        channel_->set_device_bitrate(in.board.soc.mscan->bit_rate());
        if (tick) {
            tick();
        }

        std::unique_lock<std::mutex> lock(tasks_mutex_);
        tasks_changed_.wait_for(lock, kTick, [this] { return stop_ || !tasks_.empty(); });
    }
    // Whatever was asked for in the meantime still gets an answer.
    service_tasks();
}

}  // namespace pdm32
