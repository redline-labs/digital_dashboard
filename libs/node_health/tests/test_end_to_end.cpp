// SPDX-License-Identifier: GPL-3.0-or-later
//
// A reporter and a monitor on a real session. What only a bus can show: a state
// change reaches the monitor well before the next heartbeat, and a node that
// shuts down cleanly reads as exited, not gone.
#include "node_health/monitor.h"
#include "node_health/reporter.h"

#include "pub_sub/node_identity.h"
#include "pub_sub/session_manager.h"

#include "check.h"

#include <spdlog/spdlog.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using node_health::HealthMonitor;
using node_health::HealthReporter;
using node_health::HealthRow;
using node_health::State;
using node_health::Verdict;

namespace
{

constexpr std::string_view kNode = "health_e2e";

std::optional<HealthRow> rowFor(const HealthMonitor& monitor)
{
    for (HealthRow& row : monitor.snapshot())
    {
        if (row.name == kNode)
        {
            return row;
        }
    }
    return std::nullopt;
}

// Polls until `accept` holds or `limit` passes; returns the last row seen.
std::optional<HealthRow> waitFor(const HealthMonitor& monitor, std::chrono::milliseconds limit,
                                 const std::function<bool(const HealthRow&)>& accept)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::optional<HealthRow> row;
    while (std::chrono::steady_clock::now() < deadline)
    {
        row = rowFor(monitor);
        if (row && accept(*row))
        {
            return row;
        }
        std::this_thread::sleep_for(10ms);
    }
    return row;
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    if (!pub_sub::SessionManager::getOrCreate())
    {
        SPDLOG_WARN("SKIP: no zenoh session could be opened on this host");
        return PROJECT_TEST_SKIP_CODE;
    }

    HealthMonitor monitor;
    test::check(monitor.isValid(), "the monitor subscribes");

    auto identity = std::make_unique<pub_sub::NodeIdentity>(kNode);
    // A long heartbeat, so anything that arrives quickly arrived because it
    // changed, not because a heartbeat happened to fall due.
    auto reporter = std::make_unique<HealthReporter>(kNode, node_health::ReporterOptions{5s, false});
    test::check(reporter->isPublishing(), "the reporter publishes");

    auto row = waitFor(monitor, 2s, [](const HealthRow& r) { return r.last.has_value(); });
    test::check(row && row->verdict == Verdict::starting, "a new node reads as starting");

    reporter->setCheck("device", State::ok);
    reporter->markReady();
    row = waitFor(monitor, 1s, [](const HealthRow& r) { return r.verdict == Verdict::ok; });
    test::check(row && row->verdict == Verdict::ok, "ready with an ok check reads as ok");

    const auto before_fault = std::chrono::steady_clock::now();
    reporter->setCheck("device", State::fault, "port closed");
    row = waitFor(monitor, 1s, [](const HealthRow& r) { return r.verdict == Verdict::fault; });
    const auto took = std::chrono::steady_clock::now() - before_fault;
    test::check(row && row->verdict == Verdict::fault, "a fault is reported");
    test::check(took < 1s, "without waiting for the five-second heartbeat");
    test::check(row && row->last && !row->last->checks.empty() &&
                    row->last->checks[0].detail == "port closed",
                "with the check's detail");

    row = rowFor(monitor);
    const auto late = monitor.snapshotAt(std::chrono::steady_clock::now() + 16s);
    bool saw_late = false;
    for (const HealthRow& r : late)
    {
        saw_late = saw_late || (r.name == kNode && r.verdict == Verdict::late);
    }
    test::check(saw_late, "three missed heartbeats later it would read as late");

    reporter.reset();
    identity.reset();
    row = waitFor(monitor, 2s, [](const HealthRow& r) { return r.verdict == Verdict::exited; });
    test::check(row && row->verdict == Verdict::exited,
                "a clean shutdown reads as exited: " +
                    (row ? std::string(node_health::to_string(row->verdict)) : std::string("no row")));

    return test::finish();
}
