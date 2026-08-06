#include "bag/rebuild.h"

#include "bag/validate.h"

#include <mcap/reader.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <system_error>
#include <vector>

namespace bag
{

std::optional<bag_metadata_t> rebuildMetadata(const std::string& directory)
{
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
    {
        SPDLOG_ERROR("'{}' is not a directory.", directory);
        return std::nullopt;
    }

    std::vector<std::filesystem::path> parts;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".mcap")
        {
            parts.push_back(entry.path());
        }
    }

    if (parts.empty())
    {
        SPDLOG_ERROR("No .mcap files in '{}'.", directory);
        return std::nullopt;
    }

    // By name, which is the order they were written: the writer numbers them
    // _0000, _0001, ... zero-padded, so a lexical sort is a chronological one.
    std::sort(parts.begin(), parts.end());

    bag_metadata_t metadata;
    metadata.version = 1;

    // Carried over from any surviving index -- see the header. The drop count
    // in particular exists nowhere else.
    if (const auto existing = loadMetadata(directory, /*quiet=*/true))
    {
        metadata.created = existing->created;
        metadata.recorder = existing->recorder;
        metadata.compression = existing->compression;
        metadata.dropped_messages = existing->dropped_messages;
        metadata.unstamped_messages = existing->unstamped_messages;
    }

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

    bool read_anything = false;

    for (const std::filesystem::path& part_path : parts)
    {
        mcap::McapReader reader;
        const mcap::Status status = reader.open(part_path.string());
        if (!status.ok())
        {
            SPDLOG_WARN("Skipping '{}': {}", part_path.filename().string(), status.message);
            continue;
        }

        // ForceScan rather than AllowFallbackScan: the point of a rebuild is to
        // describe what is actually in the data section. A stale or partial
        // summary is exactly what we are here to replace, so trusting it would
        // defeat the exercise.
        // Still forced, because the scan is what produces the counts below.
        const mcap::Status summary = reader.readSummary(
            mcap::ReadSummaryMethod::ForceScan,
            [&](const mcap::Status& problem)
            { SPDLOG_DEBUG("'{}': {}", part_path.filename().string(), problem.message); });

        bag_part_t part;
        part.path = part_path.filename().string();
        part.bytes = std::filesystem::file_size(part_path, error);
        if (error)
        {
            part.bytes = 0;
        }

        // "Complete" means the writer CLOSED this file, and the only proof of
        // that is the trailing magic.
        //
        // Not summary.ok(): ForceScan succeeds on a truncated part -- it scans
        // what is there and reports success -- so using it would mark every
        // torn part complete, which is exactly the fact a reader needs to know
        // and the one this is here to record. Caught by
        // bag_test_rebuild::testRebuildHandlesATornPart.
        part.complete = hasCompleteEnding(part_path.string());

        if (!summary.ok())
        {
            SPDLOG_DEBUG("'{}' scanned with problems: {}", part.path,
                         summary.message);
        }

        std::uint64_t count = 0;
        std::uint64_t t_begin = 0;
        std::uint64_t t_end = 0;

        mcap::ReadMessageOptions options;
        options.readOrder = mcap::ReadMessageOptions::ReadOrder::FileOrder;

        for (const mcap::MessageView& view :
             reader.readMessages([](const mcap::Status&) {}, options))
        {
            ++count;
            read_anything = true;

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

        metadata.parts.push_back(std::move(part));
    }

    if (metadata.parts.empty())
    {
        SPDLOG_ERROR("None of the .mcap files in '{}' could be opened.", directory);
        return std::nullopt;
    }

    // A directory of files that all opened but held no messages is legal -- an
    // empty recording -- so `read_anything` does not gate the result. It is
    // only worth a note.
    if (!read_anything)
    {
        SPDLOG_WARN("'{}' holds no messages.", directory);
    }

    for (const auto& [key, totals] : topics)
    {
        bag_topic_t topic;
        topic.key = key;
        topic.schema = totals.schema;
        topic.message_count = totals.count;
        metadata.topics.push_back(std::move(topic));
    }

    return metadata;
}

}  // namespace bag
