#include "inspect/verbs.h"

#include "inspect/traffic.h"

#include "cli/interrupt.h"
#include "cli/output.h"

#include "pub_sub/topic_directory.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <thread>

namespace inspect
{

namespace
{

std::string formatBytes(double bytes_per_second)
{
    if (bytes_per_second >= 1024.0 * 1024.0)
    {
        return fmt::format("{:.1f}M", bytes_per_second / (1024.0 * 1024.0));
    }
    if (bytes_per_second >= 1024.0)
    {
        return fmt::format("{:.1f}K", bytes_per_second / 1024.0);
    }
    if (bytes_per_second > 0.0)
    {
        return fmt::format("{:.0f}", bytes_per_second);
    }
    return "-";
}

}  // namespace

void addWatchOptions(cxxopts::Options& options)
{
    options.add_options()
        ("k,key", "Key expression to watch. Wildcards allowed.",
            cxxopts::value<std::string>()->default_value("**"))
        ("i,interval", "Seconds between refreshes.",
            cxxopts::value<double>()->default_value("1.0"))
        ("n,count", "Stop after this many refreshes.", cxxopts::value<std::uint64_t>())
        ("no-clear", "Append each refresh instead of redrawing in place.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"));

    options.parse_positional({"key"});
}

int runWatch(cli::Context& context)
{
    const std::string key = context.stringOr("key", "**");
    const double interval = context.doubleOr("interval", 1.0);
    const std::uint64_t refresh_limit = context.uintOr("count", 0);
    const bool no_clear = context.flag("no-clear") || context.json();

    if (interval <= 0.0)
    {
        SPDLOG_ERROR("--interval must be greater than zero.");
        return cli::kUsage;
    }

    // BOTH halves of the picture, which is the point of this verb.
    //
    // The directory says what EXISTS -- including topics that have never
    // published, which no amount of listening can discover. The monitor says
    // what is FLOWING -- which no advertisement can tell you, because a
    // liveliness token stays up for a CAN bridge whose adapter is unplugged.
    // Neither answers "is this bus healthy" alone; together they do, and the
    // interesting rows are exactly the ones where they disagree.
    pub_sub::TopicDirectory directory;
    pub_sub::NodeDirectory nodes;
    TrafficMonitor monitor(key);

    if (!directory.isValid())
    {
        SPDLOG_ERROR("Could not watch the advertisement space.");
        return cli::kFailure;
    }
    if (!monitor.isValid())
    {
        SPDLOG_WARN("Could not subscribe to '{}'; showing advertisements only.", key);
    }

    cli::installInterruptHandler();

    std::uint64_t refreshes = 0;

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

        std::map<std::string, KeyStats> traffic;
        for (KeyStats& stats : monitor.interval())
        {
            traffic[stats.key] = std::move(stats);
        }

        std::vector<pub_sub::DirectoryEntry> entries = directory.snapshot();

        if (context.json())
        {
            nlohmann::json out = nlohmann::json::array();
            for (const pub_sub::DirectoryEntry& entry : entries)
            {
                nlohmann::json row;
                row["key"] = entry.key;
                row["schema"] = entry.schema;
                row["reachable"] = entry.reachable;
                row["owner"] = nodes.nameFor(entry.owner_zid);
                row["owner_zid"] = entry.owner_zid;
                const auto it = traffic.find(entry.key);
                row["hz"] = it != traffic.end() ? it->second.hz : 0.0;
                row["bytes_per_second"] =
                    it != traffic.end() ? it->second.bytes_per_second : 0.0;
                out.push_back(std::move(row));
            }
            cli::out("{}", out.dump());
            cli::flush();
        }
        else
        {
            if (!no_clear)
            {
                // Cursor home + clear to end of screen. Not a full clear: that
                // scrolls the terminal's scrollback away, and losing history is
                // worse than a little flicker.
                cli::outPartial("\x1b[H\x1b[J");
            }

            std::size_t key_width = 3;
            for (const pub_sub::DirectoryEntry& entry : entries)
            {
                key_width = std::max(key_width, entry.key.size());
            }

            cli::out("{:<{}}  {:<24} {:<14} {:>9} {:>9}  {}", "TOPIC", key_width, "SCHEMA", "OWNER",
                     "HZ", "B/S", "STATE");

            for (const pub_sub::DirectoryEntry& entry : entries)
            {
                const auto it = traffic.find(entry.key);
                const bool flowing = it != traffic.end() && it->second.messages > 0;

                std::string state;
                if (!entry.reachable)
                {
                    state = "gone";
                }
                else if (!flowing)
                {
                    // The case only this verb can show: advertised, its
                    // publisher alive, and producing nothing.
                    state = "idle";
                }
                else
                {
                    state = "ok";
                }

                std::string owner = nodes.nameFor(entry.owner_zid);
                if (owner.empty())
                {
                    owner = entry.owner_zid.empty() ? "-" : "(unnamed)";
                }

                cli::out("{:<{}}  {:<24} {:<14} {:>9} {:>9}  {}", entry.key, key_width,
                         entry.schema.empty() ? "-" : entry.schema, owner,
                         flowing ? fmt::format("{:.1f}", it->second.hz) : std::string("-"),
                         flowing ? formatBytes(it->second.bytes_per_second) : std::string("-"),
                         state);
            }

            if (entries.empty())
            {
                cli::out("(nothing advertised)");
            }
            cli::flush();
        }

        ++refreshes;
        if (refresh_limit != 0 && refreshes >= refresh_limit)
        {
            break;
        }
    }

    return cli::kOk;
}

}  // namespace inspect
