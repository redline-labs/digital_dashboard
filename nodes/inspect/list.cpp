#include "inspect/verbs.h"

#include "inspect/key_match.h"
#include "inspect/traffic.h"

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


void addListOptions(cxxopts::Options& options)
{
    options.add_options()
        ("k,key", "Key expression filter. Wildcards allowed.",
            cxxopts::value<std::string>()->default_value("**"))
        ("o,observe", "ALSO listen for this many milliseconds and report traffic. Off by "
                      "default -- listing no longer requires waiting.",
            cxxopts::value<std::uint64_t>())
        ("by-node", "Group topics by the node that publishes them.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("all", "Include topics whose publisher has gone away.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"));

    options.parse_positional({"key"});
}

int runList(cli::Context& context)
{
    const std::string filter = context.stringOr("key", "**");
    const bool by_node = context.flag("by-node");
    const bool include_unreachable = context.flag("all");

    // ADVERTISEMENTS, not traffic.
    //
    // This verb used to call pub_sub::observeTopics(), which subscribes for a
    // fixed window and reports whatever happened to arrive. That has two
    // failures that cannot be fixed by tuning the window: a topic publishing
    // more slowly than the window is invisible, and a topic that exists but has
    // never published is indistinguishable from one that does not exist. Both
    // report an empty bus, which is the one answer that is never actionable.
    //
    // Every publisher declares a liveliness token at construction, and the
    // directory replays history, so this is both instant and complete.
    pub_sub::TopicDirectory directory;
    pub_sub::NodeDirectory nodes;

    if (!directory.isValid())
    {
        SPDLOG_ERROR("Could not watch the advertisement space; is a zenoh session available?");
        return cli::kFailure;
    }

    // Liveliness history arrives asynchronously. A short settle is not a
    // discovery window -- it is waiting for replies that are already in flight,
    // and it does not scale with how slowly anything publishes.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::unique_ptr<TrafficMonitor> monitor;
    if (context.has("observe"))
    {
        const std::uint64_t window_ms = context.uintOr("observe", 1000);
        monitor = std::make_unique<TrafficMonitor>(filter);
        if (!monitor->isValid())
        {
            SPDLOG_WARN("Could not subscribe to '{}'; reporting advertisements only.", filter);
            monitor.reset();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
        }
    }

    std::map<std::string, KeyStats> traffic;
    if (monitor)
    {
        for (KeyStats& stats : monitor->cumulative())
        {
            traffic[stats.key] = std::move(stats);
        }
    }

    std::vector<pub_sub::DirectoryEntry> entries;
    for (const pub_sub::DirectoryEntry& entry : directory.snapshot())
    {
        if (!keyMatches(filter, entry.key))
        {
            continue;
        }
        if (!entry.reachable && !include_unreachable)
        {
            continue;
        }
        entries.push_back(entry);
    }

    if (context.json())
    {
        nlohmann::json out = nlohmann::json::array();
        for (const pub_sub::DirectoryEntry& entry : entries)
        {
            nlohmann::json row;
            row["key"] = entry.key;
            row["schema"] = entry.schema;
            row["reachable"] = entry.reachable;
            row["appearances"] = entry.appearances;
            row["disappearances"] = entry.disappearances;
            row["owner_zid"] = entry.owner_zid;
            row["owner"] = nodes.nameFor(entry.owner_zid);
            if (const auto it = traffic.find(entry.key); it != traffic.end())
            {
                row["messages"] = it->second.messages;
                row["hz"] = it->second.hz;
                row["bytes_per_second"] = it->second.bytes_per_second;
            }
            out.push_back(std::move(row));
        }
        cli::out("{}", out.dump(2));
        return cli::kOk;
    }

    if (entries.empty())
    {
        // Said explicitly, because the old traffic-based version could not tell
        // these apart and this one can.
        cli::out("Nothing is advertised on the bus matching '{}'.", filter);
        cli::out("(This means no publisher is running -- not that none has published.)");
        return cli::kOk;
    }

    if (by_node)
    {
        std::map<std::string, std::vector<const pub_sub::DirectoryEntry*>> grouped;
        for (const pub_sub::DirectoryEntry& entry : entries)
        {
            const std::string name = nodes.nameFor(entry.owner_zid);
            std::string label;
            if (!name.empty())
            {
                label = name + " (" + entry.owner_zid + ")";
            }
            else if (!entry.owner_zid.empty())
            {
                // A zid with no name means a publisher that did not declare a
                // NodeIdentity -- not an unowned topic.
                label = "(unnamed) " + entry.owner_zid;
            }
            else
            {
                // No zid at all means an advertisement from a build that
                // predates the owner segment.
                label = "(owner not advertised)";
            }
            grouped[label].push_back(&entry);
        }

        for (const auto& [label, topics] : grouped)
        {
            cli::out("{}", label);
            for (const pub_sub::DirectoryEntry* entry : topics)
            {
                cli::out("  {:<44} {}{}", entry->key, entry->schema,
                         entry->reachable ? "" : "  [unreachable]");
            }
            cli::out("");
        }
        return cli::kOk;
    }

    std::size_t key_width = 3;
    for (const pub_sub::DirectoryEntry& entry : entries)
    {
        key_width = std::max(key_width, entry.key.size());
    }

    for (const pub_sub::DirectoryEntry& entry : entries)
    {
        std::string suffix;
        if (!entry.reachable)
        {
            suffix += "  [unreachable]";
        }
        else if (entry.disappearances > 0)
        {
            // The counters are the only signal a publisher restarted, and a
            // publisher that has been up and down repeatedly is worth noticing.
            suffix += fmt::format("  [restarted x{}]", entry.disappearances);
        }

        if (const auto it = traffic.find(entry.key); it != traffic.end())
        {
            suffix += fmt::format("  {:.1f} Hz, {} msgs", it->second.hz, it->second.messages);
        }
        else if (monitor)
        {
            suffix += "  (silent)";
        }

        // The schema column is only padded when something follows it, so a
        // plain listing has no trailing whitespace to confuse `diff` or a
        // shell-completion consumer.
        if (suffix.empty())
        {
            cli::out("{:<{}}  {}", entry.key, key_width,
                     entry.schema.empty() ? "(no schema)" : entry.schema);
        }
        else
        {
            cli::out("{:<{}}  {:<28}{}", entry.key, key_width,
                     entry.schema.empty() ? "(no schema)" : entry.schema, suffix);
        }
    }

    return cli::kOk;
}

}  // namespace inspect
