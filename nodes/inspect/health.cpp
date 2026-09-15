// SPDX-License-Identifier: GPL-3.0-or-later
#include "inspect/verbs.h"

#include "cli/interrupt.h"
#include "cli/output.h"

#include "node_health/monitor.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace inspect
{

namespace
{

using namespace std::chrono_literals;

std::string ageText(const std::optional<std::chrono::milliseconds>& age)
{
    if (!age)
    {
        return "-";
    }
    const double seconds = std::chrono::duration<double>(*age).count();
    return seconds < 10.0 ? fmt::format("{:.1f}s", seconds) : fmt::format("{:.0f}s", seconds);
}

std::string uptimeText(const std::optional<node_health::HealthSnapshot>& last)
{
    if (!last)
    {
        return "-";
    }
    const std::uint64_t seconds = last->uptime_ms / 1000u;
    if (seconds < 60u)
    {
        return fmt::format("{}s", seconds);
    }
    if (seconds < 3600u)
    {
        return fmt::format("{}m{:02}s", seconds / 60u, seconds % 60u);
    }
    return fmt::format("{}h{:02}m", seconds / 3600u, (seconds % 3600u) / 60u);
}

// The first check that is not ok, which is what a person needs to read first.
std::string problemText(const node_health::HealthRow& row)
{
    if (!row.last)
    {
        return "no health published";
    }
    for (const node_health::CheckReport& check : row.last->checks)
    {
        if (check.state == node_health::State::ok)
        {
            continue;
        }
        return check.detail.empty() ? check.name : check.name + ": " + check.detail;
    }
    return "-";
}

void printTable(const std::vector<node_health::HealthRow>& rows, bool show_checks)
{
    cli::out("{:<20} {:<9} {:>7} {:>8} {:>8}  {}", "NODE", "STATE", "AGE", "UPTIME", "RESTARTS",
             "PROBLEM");
    for (const node_health::HealthRow& row : rows)
    {
        cli::out("{:<20} {:<9} {:>7} {:>8} {:>8}  {}",
                 row.name.empty() ? row.zid.substr(0, 16) : row.name,
                 node_health::to_string(row.verdict), ageText(row.age), uptimeText(row.last),
                 row.continuity.restarts, problemText(row));
        if (!show_checks || !row.last)
        {
            continue;
        }
        for (const node_health::CheckReport& check : row.last->checks)
        {
            cli::out("  {:<18} {:<9} {}", check.name, node_health::to_string(check.state),
                     check.detail);
        }
    }
}

nlohmann::json toJson(const std::vector<node_health::HealthRow>& rows)
{
    nlohmann::json out = nlohmann::json::array();
    for (const node_health::HealthRow& row : rows)
    {
        nlohmann::json entry;
        entry["node"] = row.name;
        entry["zid"] = row.zid;
        entry["verdict"] = std::string(node_health::to_string(row.verdict));
        entry["healthy"] = node_health::isHealthy(row.verdict);
        entry["restarts"] = row.continuity.restarts;
        entry["missed_samples"] = row.continuity.missed;
        if (row.age)
        {
            entry["age_ms"] = row.age->count();
        }
        if (row.last)
        {
            entry["state"] = std::string(node_health::to_string(row.last->state));
            entry["sequence"] = row.last->sequence;
            entry["uptime_ms"] = row.last->uptime_ms;
            entry["period_ms"] = row.last->period_ms;
            entry["pid"] = row.last->pid;
            nlohmann::json checks = nlohmann::json::array();
            for (const node_health::CheckReport& check : row.last->checks)
            {
                nlohmann::json one;
                one["name"] = check.name;
                one["state"] = std::string(node_health::to_string(check.state));
                one["detail"] = check.detail;
                one["state_age_ms"] = check.state_age_ms;
                checks.push_back(std::move(one));
            }
            entry["checks"] = std::move(checks);
        }
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<node_health::HealthRow> visible(const node_health::HealthMonitor& monitor, bool include_gone)
{
    std::vector<node_health::HealthRow> rows;
    for (node_health::HealthRow& row : monitor.snapshot())
    {
        const bool gone = row.verdict == node_health::Verdict::gone ||
                          row.verdict == node_health::Verdict::exited;
        if (gone && !include_gone)
        {
            continue;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace

void addHealthOptions(cxxopts::Options& options)
{
    options.add_options()
        ("all", "Include nodes that have exited or gone away.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("checks", "List every check, not just the first problem.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("w,watch", "Keep refreshing until interrupted.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("i,interval", "Seconds between refreshes when watching.",
            cxxopts::value<double>()->default_value("1.0"))
        ("no-clear", "Append each refresh instead of redrawing in place.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("wait", "Seconds to collect heartbeats before the first report.",
            cxxopts::value<double>()->default_value("1.5"));
}

int runHealth(cli::Context& context)
{
    const bool include_gone = context.flag("all");
    const bool show_checks = context.flag("checks");
    const bool watch = context.flag("watch");
    const double interval = context.doubleOr("interval", 1.0);
    const bool no_clear = context.flag("no-clear") || context.json();
    const double wait = context.doubleOr("wait", 1.5);

    if (interval <= 0.0)
    {
        SPDLOG_ERROR("--interval must be greater than zero.");
        return cli::kUsage;
    }

    // A node is silent only if nothing arrived while this tool was listening,
    // so the grace period is the window it actually waited.
    node_health::ClassifyOptions options;
    options.silent_grace = std::chrono::milliseconds(static_cast<long long>(wait * 1000.0));

    node_health::HealthMonitor monitor(options);
    if (!monitor.isValid())
    {
        SPDLOG_ERROR("No zenoh session available.");
        return cli::kFailure;
    }

    cli::installInterruptHandler();

    // Long enough for one heartbeat from every node, since a node that has just
    // started is otherwise reported as silent.
    const auto collect = std::chrono::duration<double>(wait);
    const auto ready = std::chrono::steady_clock::now() + collect;
    while (!cli::interrupted() && std::chrono::steady_clock::now() < ready)
    {
        std::this_thread::sleep_for(20ms);
    }

    bool healthy = true;
    for (;;)
    {
        const std::vector<node_health::HealthRow> rows = visible(monitor, include_gone);
        healthy = true;
        for (const node_health::HealthRow& row : rows)
        {
            healthy = healthy && node_health::isHealthy(row.verdict);
        }

        if (context.json())
        {
            cli::out("{}", toJson(rows).dump(2));
        }
        else
        {
            if (watch && !no_clear)
            {
                // Cursor home and clear forward, like `watch`: a full clear
                // would throw away the terminal's scrollback.
                cli::outPartial("\x1b[H\x1b[J");
            }
            if (rows.empty())
            {
                cli::out("No node is publishing health.");
            }
            else
            {
                printTable(rows, show_checks);
            }
        }
        cli::flush();

        if (!watch || cli::interrupted())
        {
            break;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::duration<double>(interval);
        while (!cli::interrupted() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(20ms);
        }
    }

    // So a script can gate on it. Interrupting a watch is not a failure.
    if (watch)
    {
        return cli::kOk;
    }
    return healthy ? cli::kOk : cli::kFailure;
}

}  // namespace inspect
