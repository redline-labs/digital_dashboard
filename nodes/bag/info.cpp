#include "bag_tool/verbs.h"

#include "bag/reader.h"

#include "cli/output.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <string>

namespace bag_tool
{

namespace
{

std::string humanBytes(double bytes)
{
    if (bytes >= 1024.0 * 1024.0 * 1024.0)
    {
        return fmt::format("{:.2f} GiB", bytes / (1024.0 * 1024.0 * 1024.0));
    }
    if (bytes >= 1024.0 * 1024.0)
    {
        return fmt::format("{:.1f} MiB", bytes / (1024.0 * 1024.0));
    }
    if (bytes >= 1024.0)
    {
        return fmt::format("{:.1f} KiB", bytes / 1024.0);
    }
    return fmt::format("{:.0f} B", bytes);
}

std::string localTime(std::uint64_t unix_nanos)
{
    if (unix_nanos == 0)
    {
        return "-";
    }
    const auto seconds = static_cast<std::time_t>(unix_nanos / 1'000'000'000ull);
    std::tm parts{};
    ::localtime_r(&seconds, &parts);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
    return buffer;
}

}  // namespace

void addInfoOptions(cxxopts::Options& options)
{
    options.add_options()
        ("bag", "The recording directory.", cxxopts::value<std::string>());

    options.parse_positional({"bag"});
}

int runInfo(cli::Context& context)
{
    const auto path = context.requireString("bag");
    if (!path)
    {
        return cli::kUsage;
    }

    // FROM THE INDEX ALONE. Not one message is read, and not one part is opened.
    // That is why this is instant on a recording of any size -- which is the
    // whole reason MCAP's summary section exists and a large part of why the
    // format was chosen.
    bag::BagReader reader(*path);
    if (!reader.isValid())
    {
        return cli::kFailure;
    }

    const bag::bag_metadata_t& metadata = reader.metadata();

    const double duration =
        metadata.t_end_ns > metadata.t_begin_ns
            ? static_cast<double>(metadata.t_end_ns - metadata.t_begin_ns) / 1e9
            : 0.0;

    std::uint64_t total_bytes = 0;
    for (const bag::bag_part_t& part : metadata.parts)
    {
        total_bytes += part.bytes;
    }

    if (context.json())
    {
        nlohmann::json out;
        out["path"] = *path;
        out["created"] = metadata.created;
        out["recorder"] = metadata.recorder;
        out["compression"] = metadata.compression;
        out["duration_seconds"] = duration;
        out["t_begin_ns"] = metadata.t_begin_ns;
        out["t_end_ns"] = metadata.t_end_ns;
        out["message_count"] = metadata.message_count;
        out["bytes"] = total_bytes;
        out["dropped_messages"] = metadata.dropped_messages;
        out["unstamped_messages"] = metadata.unstamped_messages;

        out["parts"] = nlohmann::json::array();
        for (const bag::bag_part_t& part : metadata.parts)
        {
            nlohmann::json row;
            row["path"] = part.path;
            row["bytes"] = part.bytes;
            row["messages"] = part.message_count;
            row["t_begin_ns"] = part.t_begin_ns;
            row["t_end_ns"] = part.t_end_ns;
            row["complete"] = part.complete;
            out["parts"].push_back(std::move(row));
        }

        out["topics"] = nlohmann::json::array();
        for (const bag::bag_topic_t& topic : metadata.topics)
        {
            nlohmann::json row;
            row["key"] = topic.key;
            row["schema"] = topic.schema;
            row["messages"] = topic.message_count;
            row["origin_zid"] = topic.origin_zid;
            row["silent"] = topic.advertised_only;
            row["hz"] = duration > 0.0 ? static_cast<double>(topic.message_count) / duration : 0.0;
            out["topics"].push_back(std::move(row));
        }

        out["problems"] = reader.problems();
        cli::out("{}", out.dump(2));
        return cli::kOk;
    }

    cli::out("path         {}", *path);
    cli::out("recorded     {}  by {}", metadata.created,
             metadata.recorder.empty() ? "(unknown)" : metadata.recorder);
    cli::out("span         {} .. {}", localTime(metadata.t_begin_ns),
             localTime(metadata.t_end_ns));
    cli::out("duration     {:.1f}s", duration);
    cli::out("messages     {}", metadata.message_count);
    cli::out("size         {} in {} part(s), {}", humanBytes(static_cast<double>(total_bytes)),
             metadata.parts.size(), metadata.compression);

    // Both of these are the recording being honest about itself, and both are
    // printed even when zero -- a reader should not have to infer from silence
    // that nothing was lost.
    cli::out("dropped      {}{}", metadata.dropped_messages,
             metadata.dropped_messages > 0 ? "   <-- the recorder could not keep up" : "");
    cli::out("unstamped    {}{}", metadata.unstamped_messages,
             metadata.unstamped_messages > 0
                 ? "   <-- publish_time was taken from arrival for these"
                 : "");

    cli::out("");
    cli::out("PUBLISH TIME is the publisher's wall clock, so it is only as good as that clock.");
    cli::out("LOG TIME is when this recorder saw the message, and is what playback uses.");

    if (metadata.parts.size() > 1)
    {
        cli::out("");
        cli::out("parts");
        for (const bag::bag_part_t& part : metadata.parts)
        {
            cli::out("  {:<24} {:>10}  {:>9} msgs  {}{}", part.path,
                     humanBytes(static_cast<double>(part.bytes)), part.message_count,
                     localTime(part.t_begin_ns), part.complete ? "" : "  [INCOMPLETE]");
        }
    }

    cli::out("");
    cli::out("topics");

    std::vector<bag::bag_topic_t> topics = metadata.topics;
    std::sort(topics.begin(), topics.end(),
              [](const bag::bag_topic_t& lhs, const bag::bag_topic_t& rhs)
              { return lhs.key < rhs.key; });

    std::size_t key_width = 5;
    for (const bag::bag_topic_t& topic : topics)
    {
        key_width = std::max(key_width, topic.key.size());
    }

    for (const bag::bag_topic_t& topic : topics)
    {
        if (topic.advertised_only)
        {
            // The thing only an advertisement can tell you, after the fact.
            cli::out("  {:<{}}  {:<26} {:>9}  SILENT (advertised, never published)", topic.key,
                     key_width, topic.schema.empty() ? "-" : topic.schema, 0);
            continue;
        }

        const double hz =
            duration > 0.0 ? static_cast<double>(topic.message_count) / duration : 0.0;
        cli::out("  {:<{}}  {:<26} {:>9} msgs  {:>7.1f} Hz{}", topic.key, key_width,
                 topic.schema.empty() ? "-" : topic.schema, topic.message_count, hz,
                 topic.origin_zid == "(mixed)" ? "   <-- published by MORE THAN ONE session" : "");
    }

    if (!reader.problems().empty())
    {
        cli::out("");
        cli::out("problems");
        for (const std::string& problem : reader.problems())
        {
            cli::out("  {}", problem);
        }
    }

    return cli::kOk;
}

}  // namespace bag_tool
