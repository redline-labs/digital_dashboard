#include "bag_tool/verbs.h"

#include "bag/metadata.h"

#include "cli/output.h"

#include <mcap/reader.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>

namespace bag_tool
{

void addReindexOptions(cxxopts::Options& options)
{
    options.add_options()
        ("bag", "The recording directory.", cxxopts::value<std::string>())
        ("n,dry-run", "Report what would be written without writing it.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"));

    options.parse_positional({"bag"});
}

int runReindex(cli::Context& context)
{
    const auto path = context.requireString("bag");
    if (!path)
    {
        return cli::kUsage;
    }

    // WHY THIS VERB EXISTS. metadata.yaml is written when the recorder closes
    // and after every roll -- so a recorder that was killed leaves parts on disk
    // that no index describes. Without this, those parts are unreadable through
    // BagReader, which finds them through metadata.yaml and nowhere else.
    //
    // A recording that ended in a crash is the one most likely to matter, so
    // recovering it is not a nicety.
    std::error_code error;
    if (!std::filesystem::is_directory(*path, error))
    {
        SPDLOG_ERROR("'{}' is not a directory.", *path);
        return cli::kUsage;
    }

    std::vector<std::filesystem::path> parts;
    for (const auto& entry : std::filesystem::directory_iterator(*path, error))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".mcap")
        {
            parts.push_back(entry.path());
        }
    }

    if (parts.empty())
    {
        SPDLOG_ERROR("No .mcap files in '{}'.", *path);
        return cli::kFailure;
    }

    // By name, which is the order they were written: the writer numbers them
    // _0000, _0001, ... and zero-pads so a lexical sort is a chronological one.
    std::sort(parts.begin(), parts.end());

    bag::bag_metadata_t metadata;

    // Anything the previous index knew that the files cannot tell us -- the drop
    // count above all. A rebuild that reset it to zero would silently claim a
    // lossy recording was complete.
    if (const auto existing = bag::loadMetadata(*path, /*quiet=*/true))
    {
        metadata.created = existing->created;
        metadata.recorder = existing->recorder;
        metadata.compression = existing->compression;
        metadata.dropped_messages = existing->dropped_messages;
    }
    metadata.version = 1;

    // Say that this index was rebuilt rather than leaving `created` blank or
    // inventing a recording time. The parts hold the message timestamps, which
    // is the time that actually matters; when the recorder started is gone with
    // the recorder.
    if (metadata.created.empty())
    {
        metadata.created = "(rebuilt by bag reindex)";
    }
    metadata.recorder = metadata.recorder.empty()
                            ? std::string("unknown -- index rebuilt by bag reindex")
                            : metadata.recorder + " (index rebuilt by bag reindex)";

    struct TopicTotals
    {
        std::string schema;
        std::uint64_t count = 0;
    };
    std::map<std::string, TopicTotals> topics;

    for (const std::filesystem::path& part_path : parts)
    {
        mcap::McapReader reader;
        const mcap::Status status = reader.open(part_path.string());
        if (!status.ok())
        {
            SPDLOG_WARN("Skipping '{}': {}", part_path.filename().string(), status.message);
            continue;
        }

        // ForceScan rather than AllowFallbackScan: the point of reindexing is to
        // rebuild from what is actually in the data section, and a stale or
        // partial summary is exactly what we are here to replace.
        const mcap::Status summary = reader.readSummary(
            mcap::ReadSummaryMethod::ForceScan,
            [&](const mcap::Status& problem)
            { SPDLOG_DEBUG("'{}': {}", part_path.filename().string(), problem.message); });

        bag::bag_part_t part;
        part.path = part_path.filename().string();
        part.bytes = std::filesystem::file_size(part_path, error);
        if (error)
        {
            part.bytes = 0;
        }

        // A part is "complete" when its own summary was intact -- which is what
        // BagReader reports as a problem, and what tells a reader the recording
        // was cut short here.
        part.complete = summary.ok();

        std::uint64_t count = 0;
        std::uint64_t t_begin = 0;
        std::uint64_t t_end = 0;

        mcap::ReadMessageOptions options;
        options.readOrder = mcap::ReadMessageOptions::ReadOrder::FileOrder;

        for (const mcap::MessageView& view : reader.readMessages([](const mcap::Status&) {},
                                                                 options))
        {
            ++count;
            if (t_begin == 0 || view.message.logTime < t_begin)
            {
                t_begin = view.message.logTime;
            }
            t_end = std::max(t_end, view.message.logTime);

            TopicTotals& totals = topics[view.channel->topic];
            ++totals.count;
            if (totals.schema.empty())
            {
                if (const auto found = view.channel->metadata.find("redline/schema");
                    found != view.channel->metadata.end())
                {
                    totals.schema = found->second;
                }
                else if (view.schema)
                {
                    totals.schema = view.schema->name;
                }
            }
        }

        reader.close();

        part.message_count = count;
        part.t_begin_ns = t_begin;
        part.t_end_ns = t_end;

        metadata.message_count += count;
        if (t_begin != 0 && (metadata.t_begin_ns == 0 || t_begin < metadata.t_begin_ns))
        {
            metadata.t_begin_ns = t_begin;
        }
        metadata.t_end_ns = std::max(metadata.t_end_ns, t_end);

        cli::out("{:<24} {:>9} msgs{}", part.path, count, part.complete ? "" : "  [INCOMPLETE]");

        metadata.parts.push_back(std::move(part));
    }

    for (const auto& [key, totals] : topics)
    {
        bag::bag_topic_t topic;
        topic.key = key;
        topic.schema = totals.schema;
        topic.message_count = totals.count;
        metadata.topics.push_back(std::move(topic));
    }

    cli::out("");
    cli::out("{} message(s) across {} part(s), {} topic(s).", metadata.message_count,
             metadata.parts.size(), metadata.topics.size());

    // The advertisement set is gone. Only the live recorder knew which topics
    // existed without publishing, and nothing in the files records a topic that
    // never produced a message -- so a rebuilt index cannot restore that, and
    // says so rather than implying the recording had no silent topics.
    cli::out("Note: topics that were advertised but never published cannot be recovered by a "
             "rebuild -- only the recorder knew about those.");

    if (context.flag("dry-run"))
    {
        cli::out("");
        cli::out("--dry-run: nothing was written.");
        return cli::kOk;
    }

    if (!bag::saveMetadata(metadata, *path))
    {
        return cli::kFailure;
    }

    cli::out("Wrote {}.", bag::metadataPath(*path));
    return cli::kOk;
}

}  // namespace bag_tool
