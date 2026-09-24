// SPDX-License-Identifier: GPL-3.0-or-later
//
// state_estimator: the MTi's increments and the BD992's fixes in, the
// vehicle's state out, at the IMU's rate.
//
// Threads: zenoh delivers on its own threads, and a callback must not block,
// so every subscription only copies its sample into a queue. One worker owns
// the pipeline, the estimator and both publishers -- ZenohPublisher is not
// thread-safe, and the smoother is not re-entrant -- and drains the queue.
//
// --replay <bag> runs the same pipeline over a recording instead of the bus,
// as fast as it will go, and publishes what it computes: point scope at it.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/core.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/chrono.h>

#include <cxxopts.hpp>

#include "bag/reader.h"
#include "node_health/reporter.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/raw_subscriber.h"
#include "pub_sub/zenoh_publisher.h"
#include "vehicle_state.capnp.h"

#include "node_config.h"
#include "calibration_keeper.h"
#include "pipeline.h"
#include "state_fields.h"

namespace
{

std::atomic<bool> gRunning{true};

void handleSignal(int)
{
    gRunning.store(false);
}

double hostNow()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Queued
{
    std::string schema;
    std::vector<std::uint8_t> payload;
    double arrival = 0.0;
};

// What the zenoh threads hand the worker.
class Inbox
{
  public:
    void push(Queued q)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            // A worker that falls this far behind is not coming back; shed the
            // oldest rather than grow without bound.
            if (queue_.size() > 20000) queue_.pop_front();
            queue_.push_back(std::move(q));
        }
        cv_.notify_one();
    }

    std::deque<Queued> take(std::chrono::milliseconds wait)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, wait, [&] { return !queue_.empty(); });
        std::deque<Queued> out;
        out.swap(queue_);
        return out;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Queued> queue_;
};

class Outputs
{
  public:
    explicit Outputs(const state_estimator::NodeConfig& config)
        : state_(config.stateKey), status_(config.statusKey), decimation_(config.stateDecimation)
    {
    }

    void states(const std::vector<vehicle_estimator::VehicleState>& states)
    {
        for (const auto& s : states)
        {
            if (++count_ % decimation_ != 0) continue;
            state_estimator::fill(state_.fields(), s);
            state_.put();
        }
    }

    void status(const vehicle_estimator::EstimatorStatus& s, const state_estimator::CalibrationReport& c = {})
    {
        state_estimator::fill(status_.fields(), s, c);
        status_.put();
    }

  private:
    pub_sub::ZenohPublisher<::VehicleState> state_;
    pub_sub::ZenohPublisher<::VehicleEstimatorStatus> status_;
    unsigned decimation_;
    std::uint64_t count_ = 0;
};

std::string sessionId()
{
    const auto now = std::chrono::system_clock::now();
    return fmt::format("{:%Y%m%dT%H%M%S}-{}", std::chrono::floor<std::chrono::seconds>(now), ::getpid());
}

// The calibration health: whether what is learned is being kept, and whether
// anything has moved since it was.
void calibrationHealth(node_health::HealthReporter& health, const state_estimator::CalibrationReport& c, bool enabled)
{
    static constexpr std::array<const char*, 5> kNames{"mounting", "lever arm", "boresight", "magnetometer",
                                                       "barometer airflow"};
    if (!enabled)
    {
        health.setCheck("calibration", node_health::State::ok, "not kept (disabled in the config)");
        return;
    }
    if (!c.store_open)
    {
        health.setCheck("calibration", node_health::State::degraded, "not kept: " + c.store_error);
        return;
    }
    // Degraded for the whole session: someone should look at the moved file.
    if (!c.store_recovered.empty())
    {
        health.setCheck("calibration", node_health::State::degraded,
                        "store was damaged; moved to " + c.store_recovered + ", started afresh");
        return;
    }
    for (std::size_t i = 0; i < kNames.size(); ++i)
        if (c.moved[i])
        {
            health.setCheck("calibration", node_health::State::degraded,
                            fmt::format("{} is {:.1f} sigma from last session's: moved?", kNames[i], c.moved_by[i]));
            return;
        }
    health.setCheck("calibration", node_health::State::ok, fmt::format("{} rows written", c.rows_written));
}

int replay(const state_estimator::NodeConfig& config, const std::string& path,
           const std::optional<std::string>& calibration_db)
{
    bag::BagReader reader(path);
    if (!reader.isValid())
    {
        for (const auto& p : reader.problems()) SPDLOG_ERROR("{}: {}", path, p);
        return 1;
    }
    state_estimator::Pipeline pipeline(config);
    Outputs outputs(config);
    // A replay reads and writes the calibration store only when told which:
    // an old recording must not quietly add rows to the car's live history.
    std::optional<state_estimator::CalibrationKeeper> keeper;
    if (calibration_db)
    {
        keeper.emplace(config, sessionId() + "-replay");
        if (!keeper->open(*calibration_db))
        {
            SPDLOG_ERROR("calibration store {}: {}", *calibration_db, keeper->report().store_error);
            return 1;
        }
        keeper->seed(pipeline.estimator());
    }
    std::uint64_t used = 0, states = 0, seen = 0;
    reader.forEach([&](const bag::BagMessage& m) {
        const double arrival = static_cast<double>(m.log_time_ns) * 1e-9;
        if (pipeline.onMessage(m.schema, m.payload, arrival) == state_estimator::Fed::used) ++used;
        const auto out = pipeline.advance(arrival);
        states += out.size();
        outputs.states(out);
        if (keeper && pipeline.estimator().status().keyframes != seen)
        {
            seen = pipeline.estimator().status().keyframes;
            keeper->tick(pipeline.estimator());
        }
        return gRunning.load();
    });
    if (keeper) keeper->tick(pipeline.estimator(), true);
    outputs.status(pipeline.estimator().status(), keeper ? keeper->report() : state_estimator::CalibrationReport{});
    const auto& st = pipeline.estimator().status();
    SPDLOG_INFO("replay: {} messages used, {} malformed; {} keyframes, {} states, {} resets", used,
                pipeline.malformed(), st.keyframes, states, st.resets);
    return st.initialized ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    core::setupLogging({.program = "state_estimator"});

    cxxopts::Options options("state_estimator", "Vehicle state estimator: IMU + dual-antenna GNSS fixed-lag smoother");
    options.add_options()("c,config", "Config file", cxxopts::value<std::string>()->default_value(
                                                          "configs/state_estimator/state_estimator.yaml"))(
        "check", "Validate the config and exit")("replay", "Run over a bag directory instead of the bus",
                                                  cxxopts::value<std::string>())(
        "calibration-db", "With --replay: the calibration store to read and write (none by default)",
        cxxopts::value<std::string>())(
        "debug", "Debug logging")("h,help", "Help");
    cxxopts::ParseResult args;
    try
    {
        args = options.parse(argc, argv);
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        return 2;
    }
    if (args.count("help"))
    {
        fmt::print("{}\n", options.help());
        return 0;
    }
    if (args.count("debug")) spdlog::set_level(spdlog::level::debug);

    state_estimator::NodeConfig config;
    if (!state_estimator::load_node_config(args["config"].as<std::string>(), config)) return 1;
    if (args.count("check"))
    {
        SPDLOG_INFO("config ok: IMU from '{}', GNSS from '{}', state on '{}'", config.imuPrefix, config.gnssPrefix,
                    config.stateKey);
        return 0;
    }

    // Before any publisher, so a tool watching the bus sees the node appear first.
    pub_sub::NodeIdentity identity("state_estimator");
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    if (args.count("replay"))
        return replay(config, args["replay"].as<std::string>(),
                      args.count("calibration-db") ? std::optional(args["calibration-db"].as<std::string>())
                                                   : std::nullopt);
    if (args.count("calibration-db"))
    {
        SPDLOG_ERROR("--calibration-db is for --replay; the live node uses calibration.database from the config");
        return 2;
    }

    node_health::HealthReporter health("state_estimator");
    auto& imu_seen = health.addActivityCheck("imu", std::chrono::milliseconds(200));
    auto& gnss_seen = health.addActivityCheck("gnss", std::chrono::milliseconds(1000));

    Inbox inbox;
    const auto subscribe = [&](const std::string& prefix, node_health::ActivityCheck& seen) {
        return std::make_unique<pub_sub::RawSubscriber>(
            prefix + "/**", [&inbox, &seen](const std::vector<std::uint8_t>& payload, std::string_view schema) {
                seen.touch();
                inbox.push(Queued{std::string(schema), payload, hostNow()});
            });
    };

    std::thread worker([&] {
        state_estimator::Pipeline pipeline(config);
        Outputs outputs(config);
        state_estimator::CalibrationKeeper keeper(config, sessionId());
        if (config.calibration.enabled)
        {
            const auto db = state_estimator::CalibrationKeeper::databasePath(config);
            if (keeper.open(db))
            {
                SPDLOG_INFO("calibration kept in {}", db.string());
                keeper.seed(pipeline.estimator());
            }
            else
            {
                SPDLOG_ERROR("calibration store {}: {}; running on the config's priors", db.string(),
                             keeper.report().store_error);
            }
        }
        calibrationHealth(health, keeper.report(), config.calibration.enabled);
        auto next_status = std::chrono::steady_clock::now();
        while (gRunning.load())
        {
            for (auto& q : inbox.take(std::chrono::milliseconds(20)))
                pipeline.onMessage(q.schema, q.payload, q.arrival);
            outputs.states(pipeline.advance(hostNow()));

            const auto now = std::chrono::steady_clock::now();
            if (now < next_status) continue;
            next_status = now + std::chrono::seconds(1);
            const auto& st = pipeline.estimator().status();
            keeper.tick(pipeline.estimator());
            outputs.status(st, keeper.report());
            calibrationHealth(health, keeper.report(), config.calibration.enabled);
            const auto latest = pipeline.estimator().latest();
            if (st.anchored)
                health.setCheck("estimate", node_health::State::degraded,
                                latest && latest->heading_magnetic
                                    ? "no GNSS position: attitude only, heading magnetic"
                                    : "no GNSS position: roll and pitch only");
            else if (!st.initialized)
                health.setCheck("estimate", node_health::State::degraded, "waiting for a dual-antenna heading");
            else if (!latest || !latest->valid)
                health.setCheck("estimate", node_health::State::degraded, "uncertainty above the valid bounds");
            else
                health.setCheck("estimate", node_health::State::ok);
            // A smoother that cannot keep up with 10 Hz keyframes falls
            // behind the car.
            health.setCheck("solve", st.last_solve_ms < 80.0 ? node_health::State::ok : node_health::State::degraded,
                            fmt::format("{:.1f} ms", st.last_solve_ms));
        }
        // The last chance to keep what this session learned.
        keeper.tick(pipeline.estimator(), true);
    });

    // Declared after the worker's captures and before the loop; destroyed
    // (joining any in-flight callback) before the inbox they push into.
    const auto imu_sub = subscribe(config.imuPrefix, imu_seen);
    const auto gnss_sub = subscribe(config.gnssPrefix, gnss_seen);
    if (!imu_sub->isValid() || !gnss_sub->isValid())
    {
        SPDLOG_ERROR("could not subscribe");
        gRunning.store(false);
    }

    health.markReady();
    while (gRunning.load())
    {
        health.kick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    worker.join();
    return 0;
}
