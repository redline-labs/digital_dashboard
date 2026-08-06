#include "inspect/verbs.h"

#include "inspect/traffic.h"

#include "cli/interrupt.h"
#include "cli/output.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace inspect
{

namespace
{

// hz, bw and latency are one measurement reported three ways.
//
// They all subscribe to a key expression, tabulate what arrives, and print a
// row per key on an interval. Writing them separately would mean three copies of
// the loop, the interrupt handling, the JSON mode and the "nothing arrived"
// case -- and three chances for them to disagree about what a second is.
enum class Report
{
    Rate,
    Bandwidth,
    Latency,
};

void addCommonOptions(cxxopts::Options& options)
{
    options.add_options()
        ("k,key", "Key expression to measure. Wildcards allowed.",
            cxxopts::value<std::string>()->default_value("**"))
        ("i,interval", "Seconds between reports.",
            cxxopts::value<double>()->default_value("1.0"))
        ("n,count", "Stop after this many reports.", cxxopts::value<std::uint64_t>());

    options.parse_positional({"key"});
}

std::string formatSeconds(double seconds)
{
    if (seconds >= 1.0)
    {
        return fmt::format("{:.3f}s", seconds);
    }
    if (seconds >= 0.001)
    {
        return fmt::format("{:.2f}ms", seconds * 1e3);
    }
    return fmt::format("{:.0f}us", seconds * 1e6);
}

std::string formatBytes(double bytes_per_second)
{
    if (bytes_per_second >= 1024.0 * 1024.0)
    {
        return fmt::format("{:.2f} MiB/s", bytes_per_second / (1024.0 * 1024.0));
    }
    if (bytes_per_second >= 1024.0)
    {
        return fmt::format("{:.2f} KiB/s", bytes_per_second / 1024.0);
    }
    return fmt::format("{:.0f} B/s", bytes_per_second);
}

void printRow(Report report, const KeyStats& stats)
{
    switch (report)
    {
        case Report::Rate:
            if (stats.period_samples == 0)
            {
                // The very first message on a key, with nothing before it to
                // measure against. Saying so beats printing "0.000s ± 0.000s",
                // which reads like a perfectly regular publisher.
                cli::out("{:<44} {:>8.2f} Hz  {:>6} msgs  (first message -- no period yet)",
                         stats.key, stats.hz, stats.messages);
            }
            else
            {
                cli::out("{:<44} {:>8.2f} Hz  {:>6} msgs  period {} min {} max {} sd {}",
                         stats.key, stats.hz, stats.messages, formatSeconds(stats.period_mean),
                         formatSeconds(stats.period_min), formatSeconds(stats.period_max),
                         formatSeconds(stats.period_stddev));
            }
            break;

        case Report::Bandwidth:
            cli::out("{:<44} {:>12}  {:>6} msgs  {:>8.0f} B/msg", stats.key,
                     formatBytes(stats.bytes_per_second), stats.messages,
                     stats.messages == 0
                         ? 0.0
                         : static_cast<double>(stats.bytes) / static_cast<double>(stats.messages));
            break;

        case Report::Latency:
            if (stats.stamped == 0)
            {
                // Never printed as 0.0: a topic whose samples carry no timestamp
                // has no measurable latency, and showing zero would claim an
                // instant link.
                cli::out("{:<44} {:>6} msgs  UNSTAMPED -- no publish time to compare against",
                         stats.key, stats.messages);
            }
            else
            {
                cli::out("{:<44} mean {} min {} max {} p99 {}  ({}/{} stamped)", stats.key,
                         formatSeconds(stats.latency_mean), formatSeconds(stats.latency_min),
                         formatSeconds(stats.latency_max), formatSeconds(stats.latency_p99),
                         stats.stamped, stats.messages);
            }
            break;
    }
}

nlohmann::json toJson(Report report, const KeyStats& stats)
{
    nlohmann::json row;
    row["key"] = stats.key;
    row["schema"] = stats.schema;
    row["messages"] = stats.messages;
    row["interval_seconds"] = stats.interval_seconds;

    switch (report)
    {
        case Report::Rate:
            row["hz"] = stats.hz;
            row["period_samples"] = stats.period_samples;
            if (stats.period_samples > 0)
            {
                row["period_mean"] = stats.period_mean;
                row["period_min"] = stats.period_min;
                row["period_max"] = stats.period_max;
                row["period_stddev"] = stats.period_stddev;
            }
            break;
        case Report::Bandwidth:
            row["bytes"] = stats.bytes;
            row["bytes_per_second"] = stats.bytes_per_second;
            break;
        case Report::Latency:
            row["stamped"] = stats.stamped;
            if (stats.stamped > 0)
            {
                row["latency_mean"] = stats.latency_mean;
                row["latency_min"] = stats.latency_min;
                row["latency_max"] = stats.latency_max;
                row["latency_p99"] = stats.latency_p99;
            }
            break;
    }

    return row;
}

int run(Report report, cli::Context& context)
{
    const std::string key = context.stringOr("key", "**");
    const double interval = context.doubleOr("interval", 1.0);
    const std::uint64_t report_limit = context.uintOr("count", 0);

    if (interval <= 0.0)
    {
        SPDLOG_ERROR("--interval must be greater than zero.");
        return cli::kUsage;
    }

    TrafficMonitor monitor(key);
    if (!monitor.isValid())
    {
        SPDLOG_ERROR("Could not subscribe to '{}'.", key);
        return cli::kFailure;
    }

    if (report == Report::Latency)
    {
        SPDLOG_INFO("Latency is arrival minus the publisher's timestamp, so it measures the "
                    "publisher's clock as much as the link. See pub_sub/timestamp.h.");
    }

    if (report == Report::Bandwidth)
    {
        // Worth saying, because the obvious use of this verb -- "how much did
        // enabling zenoh timestamping cost?" -- is one it CANNOT answer. The
        // timestamp rides in zenoh's framing, which a subscriber never sees;
        // what arrives here is the capnp payload alone. Measuring the wire needs
        // a packet capture, not this.
        SPDLOG_DEBUG("Payload bytes only. Zenoh's own framing -- including the sample timestamp "
                     "-- is not visible to a subscriber and is not counted here.");
    }

    cli::installInterruptHandler();

    std::uint64_t reports = 0;
    bool saw_anything = false;

    while (!cli::interrupted())
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::duration<double>(interval);
        while (!cli::interrupted() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (cli::interrupted())
        {
            break;
        }

        std::vector<KeyStats> stats = monitor.interval();
        std::sort(stats.begin(), stats.end(),
                  [](const KeyStats& lhs, const KeyStats& rhs) { return lhs.key < rhs.key; });

        if (context.json())
        {
            nlohmann::json out = nlohmann::json::array();
            for (const KeyStats& row : stats)
            {
                out.push_back(toJson(report, row));
            }
            cli::out("{}", out.dump());
        }
        else if (stats.empty())
        {
            cli::out("(no traffic on '{}')", key);
        }
        else
        {
            for (const KeyStats& row : stats)
            {
                printRow(report, row);
            }
        }
        cli::flush();

        saw_anything = saw_anything || !stats.empty();

        ++reports;
        if (report_limit != 0 && reports >= report_limit)
        {
            break;
        }
    }

    // Nothing at all arrived. Not an error -- an idle bus is a legitimate state
    // -- but worth pointing at `list`, which can tell an idle topic from an
    // absent one and this cannot.
    if (!saw_anything)
    {
        SPDLOG_INFO("Nothing published on '{}'. `inspect list` will say whether anything "
                    "advertises it.", key);
    }

    return cli::kOk;
}

}  // namespace

void addHzOptions(cxxopts::Options& options)
{
    addCommonOptions(options);
}

int runHz(cli::Context& context)
{
    return run(Report::Rate, context);
}

void addBwOptions(cxxopts::Options& options)
{
    addCommonOptions(options);
}

int runBw(cli::Context& context)
{
    return run(Report::Bandwidth, context);
}

void addLatencyOptions(cxxopts::Options& options)
{
    addCommonOptions(options);
}

int runLatency(cli::Context& context)
{
    return run(Report::Latency, context);
}

}  // namespace inspect
