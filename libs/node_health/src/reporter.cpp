// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/reporter.h"

#include "node_health/classify.h"
#include "node_health/codec.h"

#include "core/core.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace node_health
{

namespace
{

// A burst of state changes becomes one sample, not one each.
constexpr std::chrono::milliseconds kMinChangeGap{100};
// How often activity checks are re-evaluated between heartbeats.
constexpr std::chrono::milliseconds kEvaluateEvery{100};

}  // namespace

ActivityCheck::ActivityCheck(std::string name, std::chrono::milliseconds within, State when_silent,
                             Clock::time_point created)
    : name_(std::move(name))
    , within_(within)
    , when_silent_(when_silent)
    , created_(created)
{
}

State ActivityCheck::evaluate(Clock::time_point now, std::string& detail) const
{
    std::optional<Clock::time_point> last;
    if (touched_.load(std::memory_order_relaxed))
    {
        last = Clock::time_point(Clock::duration(last_touch_.load(std::memory_order_relaxed)));
    }
    return activityState(last, created_, within_, when_silent_, now, detail);
}

struct HealthReporter::Impl
{
    Impl(std::string_view node_name, ReporterOptions reporter_options)
        : name(node_name)
        , options(reporter_options)
        , started(Clock::now())
        , publisher("nodes/" + std::string(node_name) + "/health")
        , zid(pub_sub::SessionManager::zid())
        , pid(static_cast<std::uint32_t>(::getpid()))
    {
        if (options.period < kMinChangeGap)
        {
            options.period = kMinChangeGap;
        }
        if (!publisher.isValid())
        {
            SPDLOG_WARN("[health] {}: no bus session; checks run but nothing is published", name);
        }
        if (options.systemd)
        {
            watchdog = core::systemd::watchdogInterval();
        }
        thread = std::thread([this] { run(); });
    }

    // Everything below runs with `mutex` held unless it says otherwise.

    State overallLocked() const
    {
        if (stopping)
        {
            return State::stopping;
        }
        if (!ready)
        {
            return State::starting;
        }
        return worst(checks);
    }

    void setLocked(std::string_view check_name, State state, std::string_view detail, Clock::time_point now)
    {
        const auto it = std::find_if(checks.begin(), checks.end(),
                                     [&](const Check& check) { return check.name == check_name; });
        if (it == checks.end())
        {
            checks.push_back(Check{std::string(check_name), state, std::string(detail), now});
            markChangedLocked();
            return;
        }
        if (it->state != state)
        {
            it->state = state;
            it->since = now;
            markChangedLocked();
        }
        it->detail = detail;
    }

    void markChangedLocked()
    {
        changed = true;
        ++generation;
        wake.notify_one();
    }

    void refreshActivityLocked(Clock::time_point now)
    {
        std::string detail;
        for (const auto& activity : activities)
        {
            const State state = activity->evaluate(now, detail);
            setLocked(activity->name(), state, detail, now);
        }
    }

    void publishLocked(Clock::time_point now)
    {
        const State state = overallLocked();

        HealthSnapshot snapshot;
        snapshot.node = name;
        snapshot.zid = zid;
        snapshot.state = state;
        snapshot.sequence = ++sequence;
        snapshot.uptime_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
        snapshot.period_ms = static_cast<std::uint32_t>(options.period.count());
        snapshot.pid = pid;
        snapshot.checks.reserve(checks.size());
        for (const Check& check : checks)
        {
            snapshot.checks.push_back(CheckReport{
                .name = check.name,
                .state = check.state,
                .detail = check.detail,
                .state_age_ms = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - check.since).count()),
            });
        }

        if (publisher.isValid())
        {
            encode(publisher.fields(), snapshot);
            publisher.put();
        }
        last_publish = now;
        changed = false;

        if (options.systemd)
        {
            reportStatusLocked(state);
        }
    }

    // STATUS= on a change of overall state or of the first failing check, so
    // `systemctl status` says what is wrong without a flood of datagrams.
    void reportStatusLocked(State state)
    {
        std::string status(to_string(state));
        for (const Check& check : checks)
        {
            if (check.state != State::ok)
            {
                status += ": " + check.name;
                if (!check.detail.empty())
                {
                    status += ": " + check.detail;
                }
                break;
            }
        }
        if (status != last_status)
        {
            core::systemd::notifyStatus(status);
            last_status = std::move(status);
        }
    }

    void feedWatchdog(Clock::time_point now)
    {
        if (!watchdog || now - last_watchdog < *watchdog / 2)
        {
            return;
        }
        const bool kicked = kicked_ever.load(std::memory_order_relaxed);
        const Clock::time_point last_kick_time(Clock::duration(last_kick.load(std::memory_order_relaxed)));
        if (!kicked || now - last_kick_time <= 2 * options.period)
        {
            core::systemd::notifyWatchdog();
        }
        last_watchdog = now;
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        publishLocked(Clock::now());
        while (!stop_thread)
        {
            Clock::time_point deadline = last_publish + options.period;
            if (changed)
            {
                deadline = std::min(deadline, last_publish + kMinChangeGap);
            }
            deadline = std::min(deadline, Clock::now() + kEvaluateEvery);
            const std::uint64_t seen = generation;
            wake.wait_until(lock, deadline, [&] { return stop_thread || generation != seen; });
            if (stop_thread)
            {
                break;
            }

            const Clock::time_point now = Clock::now();
            refreshActivityLocked(now);
            const bool heartbeat_due = now - last_publish >= options.period;
            const bool change_due = changed && now - last_publish >= kMinChangeGap;
            if (heartbeat_due || change_due)
            {
                publishLocked(now);
            }
            feedWatchdog(now);
        }
    }

    std::string name;
    ReporterOptions options;
    Clock::time_point started;
    pub_sub::ZenohPublisher<::NodeHealth> publisher;
    std::string zid;
    std::uint32_t pid;
    std::optional<std::chrono::microseconds> watchdog;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::vector<Check> checks;
    std::deque<std::unique_ptr<ActivityCheck>> activities;
    bool ready = false;
    bool stopping = false;
    bool stop_thread = false;
    bool changed = false;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    Clock::time_point last_publish{};
    Clock::time_point last_watchdog{};
    std::string last_status;

    std::atomic<bool> kicked_ever{false};
    std::atomic<Clock::rep> last_kick{0};

    // Last, so it is started after everything it reads exists.
    std::thread thread;
};

HealthReporter::HealthReporter(std::string_view node_name, ReporterOptions options)
    : impl_(std::make_unique<Impl>(node_name, options))
{
}

HealthReporter::~HealthReporter()
{
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
        impl_->publishLocked(Clock::now());
        impl_->stop_thread = true;
    }
    impl_->wake.notify_one();
    impl_->thread.join();
}

void HealthReporter::setCheck(std::string_view name, State state, std::string_view detail)
{
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->setLocked(name, state, detail, Clock::now());
}

void HealthReporter::clearCheck(std::string_view name)
{
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto removed = std::erase_if(impl_->checks, [&](const Check& check) { return check.name == name; });
    if (removed > 0)
    {
        impl_->markChangedLocked();
    }
}

ActivityCheck& HealthReporter::addActivityCheck(std::string_view name, std::chrono::milliseconds within,
                                                State when_silent)
{
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const Clock::time_point now = Clock::now();
    impl_->activities.push_back(std::make_unique<ActivityCheck>(std::string(name), within, when_silent, now));
    impl_->setLocked(name, State::starting, {}, now);
    return *impl_->activities.back();
}

void HealthReporter::markReady()
{
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->ready)
        {
            return;
        }
        impl_->ready = true;
        impl_->markChangedLocked();
    }
    if (impl_->options.systemd)
    {
        core::systemd::notifyReady();
    }
}

void HealthReporter::kick() noexcept
{
    impl_->last_kick.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    impl_->kicked_ever.store(true, std::memory_order_relaxed);
}

State HealthReporter::overall() const
{
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->overallLocked();
}

bool HealthReporter::isPublishing() const
{
    return impl_->publisher.isValid();
}

}  // namespace node_health
