// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node side: keep a set of named checks current and publish them.
//
//     node_health::HealthReporter health("megasquirt");
//     auto& can_rx = health.addActivityCheck("can_rx", std::chrono::seconds(1));
//     ... subscriber callback: can_rx.touch();
//     health.markReady();
//     while (!cli::interrupted()) { sleep_for(100ms); health.kick(); }
//
// Declare it BEFORE anything whose callbacks touch its checks, so those are
// destroyed first and cannot touch a check that no longer exists.
#ifndef NODE_HEALTH_REPORTER_H_
#define NODE_HEALTH_REPORTER_H_

#include "node_health/state.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace node_health
{

struct ReporterOptions
{
    // The heartbeat. A monitor calls the node late after a few of these pass
    // with no sample.
    std::chrono::milliseconds period{1000};
    // Send READY=1, STATUS= and WATCHDOG=1 to systemd when running under a
    // unit. Harmless outside one.
    bool systemd = true;
};

// A check that is ok while something keeps happening: frames arriving,
// samples being decoded. touch() is lock-free and safe from any thread,
// including a zenoh callback.
class ActivityCheck
{
  public:
    ActivityCheck(std::string name, std::chrono::milliseconds within, State when_silent,
                  Clock::time_point created);

    void touch() noexcept
    {
        last_touch_.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        touched_.store(true, std::memory_order_relaxed);
    }

    const std::string& name() const { return name_; }

    // The check's state at `now`, with `detail` filled in when it is not ok.
    State evaluate(Clock::time_point now, std::string& detail) const;

  private:
    std::string name_;
    std::chrono::milliseconds within_;
    State when_silent_;
    Clock::time_point created_;
    std::atomic<Clock::rep> last_touch_{0};
    std::atomic<bool> touched_{false};
};

class HealthReporter
{
  public:
    explicit HealthReporter(std::string_view node_name, ReporterOptions options = {});
    // Publishes one last sample saying `stopping`, then stops the thread.
    ~HealthReporter();

    HealthReporter(const HealthReporter&) = delete;
    HealthReporter& operator=(const HealthReporter&) = delete;
    HealthReporter(HealthReporter&&) = delete;
    HealthReporter& operator=(HealthReporter&&) = delete;

    // Adds or updates a check. A change of state is published at once
    // (at most one sample per 100 ms); a change of detail alone waits for the
    // next heartbeat.
    void setCheck(std::string_view name, State state, std::string_view detail = {});
    void clearCheck(std::string_view name);

    // A check evaluated on the reporter's thread from touch() times. The
    // reference stays valid for the reporter's lifetime.
    ActivityCheck& addActivityCheck(std::string_view name, std::chrono::milliseconds within,
                                    State when_silent = State::degraded);

    // Leaves `starting`, and tells systemd the unit is ready.
    void markReady();

    // Proof the main loop is turning. Once a node has kicked, the systemd
    // watchdog is only fed while kicks keep arriving (within two periods), so
    // a hung main loop gets the unit restarted. A node that never kicks is fed
    // by the reporter alone.
    void kick() noexcept;

    State overall() const;

    // False when there is no bus session: checks and systemd notifications
    // still run, nothing is published.
    bool isPublishing() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace node_health

#endif  // NODE_HEALTH_REPORTER_H_
