#include "inspect/verbs.h"

#include "inspect/traffic.h"

#include "inspect/describe.h"

#include "cli/output.h"

#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/topic_directory.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <thread>

namespace inspect
{

void addInfoOptions(cxxopts::Options& options)
{
    options.add_options()
        ("k,key", "The topic to describe.", cxxopts::value<std::string>())
        ("o,observe", "ALSO listen for this many milliseconds and report traffic.",
            cxxopts::value<std::uint64_t>());

    options.parse_positional({"key"});
}

int runInfo(cli::Context& context)
{
    const auto key = context.requireString("key");
    if (!key)
    {
        return cli::kUsage;
    }

    // FROM THE ADVERTISEMENT, not from a sample.
    //
    // This verb used to declare a subscriber and wait up to two seconds for a
    // message, purely to read the schema name off its encoding. That meant it
    // took two seconds to say "unknown" about any topic publishing more slowly
    // than that -- and said "unknown" about a topic that was perfectly well
    // advertised and simply idle. The advertisement carries the schema, so this
    // is both instant and correct for an idle topic.
    pub_sub::TopicDirectory directory;
    pub_sub::NodeDirectory nodes;

    if (!directory.isValid())
    {
        SPDLOG_ERROR("Could not watch the advertisement space; is a zenoh session available?");
        return cli::kFailure;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto entries = directory.snapshot();
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const pub_sub::DirectoryEntry& e) { return e.key == *key; });

    if (found == entries.end())
    {
        SPDLOG_ERROR("'{}' is not advertised. No publisher for it is running.", *key);
        // Not kFailure: the command worked, the answer is "nothing there". A
        // script distinguishing "the tool broke" from "the topic is absent"
        // needs those to differ, and absence is the answer to a question about
        // a topic that may legitimately not be up.
        return cli::kOk;
    }

    std::optional<KeyStats> traffic;
    if (context.has("observe"))
    {
        const std::uint64_t window_ms = context.uintOr("observe", 1000);
        TrafficMonitor monitor(*key);
        if (monitor.isValid())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
            for (KeyStats& stats : monitor.cumulative())
            {
                if (stats.key == *key)
                {
                    traffic = std::move(stats);
                }
            }
        }
    }

    const std::string owner_name = nodes.nameFor(found->owner_zid);
    const auto schema = pub_sub::get_schema(found->schema);

    if (context.json())
    {
        nlohmann::json out;
        out["key"] = found->key;
        out["schema"] = found->schema;
        out["reachable"] = found->reachable;
        out["appearances"] = found->appearances;
        out["disappearances"] = found->disappearances;
        out["owner_zid"] = found->owner_zid;
        out["owner"] = owner_name;
        if (schema)
        {
            out["fields"] = pub_sub::describeSchema(*schema);
        }
        if (traffic)
        {
            out["hz"] = traffic->hz;
            out["messages"] = traffic->messages;
            out["bytes_per_second"] = traffic->bytes_per_second;
        }
        cli::out("{}", out.dump(2));
        return cli::kOk;
    }

    cli::out("key        {}", found->key);
    cli::out("schema     {}", found->schema.empty() ? "(none advertised)" : found->schema);
    cli::out("reachable  {}", found->reachable ? "yes" : "no");

    if (found->owner_zid.empty())
    {
        cli::out("owner      (not advertised -- publisher predates the owner segment)");
    }
    else if (owner_name.empty())
    {
        cli::out("owner      {} (no node name declared)", found->owner_zid);
    }
    else
    {
        cli::out("owner      {} ({})", owner_name, found->owner_zid);
    }

    if (found->disappearances > 0)
    {
        cli::out("history    up {} time(s), gone {} time(s)", found->appearances,
                 found->disappearances);
    }

    if (traffic)
    {
        if (traffic->messages == 0)
        {
            cli::out("traffic    nothing during the observation window");
        }
        else
        {
            cli::out("traffic    {} msgs, {:.1f} Hz, {:.0f} B/s", traffic->messages, traffic->hz,
                     traffic->bytes_per_second);
        }
    }

    if (!schema)
    {
        cli::out("");
        cli::out("Schema '{}' is not in this build's registry, so its fields are unknown.",
                 found->schema);
        return cli::kOk;
    }

    cli::out("");
    cli::out("fields");
    printSchemaFields(pub_sub::describeSchema(*schema), "  ");

    return cli::kOk;
}

}  // namespace inspect
