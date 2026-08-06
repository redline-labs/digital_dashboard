#include "bag/reader.h"

#include <mcap/reader.hpp>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <map>
#include <system_error>

namespace bag
{

struct BagReader::Impl
{
    std::string directory;
    bag_metadata_t metadata;
    std::vector<std::string> problems;
    bool valid = false;

    // Registry schema name -> the descriptor bytes stored in the recording.
    // Filled on first use rather than in the constructor, so opening a bag to
    // read its index does not pay for opening every part.
    mutable std::map<std::string, std::vector<std::uint8_t>> descriptors;
    mutable bool descriptors_loaded = false;

    void loadDescriptors() const
    {
        if (descriptors_loaded)
        {
            return;
        }
        descriptors_loaded = true;

        for (const bag_part_t& part : metadata.parts)
        {
            const std::string path =
                (std::filesystem::path(directory) / part.path).string();

            std::error_code error;
            if (!std::filesystem::exists(path, error))
            {
                continue;
            }

            mcap::McapReader reader;
            if (!reader.open(path).ok())
            {
                continue;
            }

            // AllowFallbackScan so a torn part still yields its schemas -- the
            // definitions live in the data section, so a missing summary is no
            // reason to lose them.
            (void)reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan,
                                     [](const mcap::Status&) {});

            // Channels carry our registry name; the Schema record carries
            // capnp's qualified one. Going through the channel is what lets a
            // caller ask by the name the rest of the tree uses.
            for (const auto& [channel_id, channel] : reader.channels())
            {
                const auto named = channel->metadata.find("redline/schema");
                if (named == channel->metadata.end() || channel->schemaId == 0)
                {
                    continue;
                }
                if (descriptors.count(named->second) != 0)
                {
                    continue;
                }

                const auto& schemas = reader.schemas();
                const auto found = schemas.find(channel->schemaId);
                if (found == schemas.end() || found->second->data.empty())
                {
                    continue;
                }

                const auto& data = found->second->data;
                descriptors.emplace(named->second,
                                    std::vector<std::uint8_t>(
                                        reinterpret_cast<const std::uint8_t*>(data.data()),
                                        reinterpret_cast<const std::uint8_t*>(data.data() +
                                                                              data.size())));
            }

            reader.close();
        }
    }
};

BagReader::BagReader(std::string directory) : impl_(std::make_unique<Impl>())
{
    impl_->directory = std::move(directory);

    const auto loaded = loadMetadata(impl_->directory);
    if (!loaded)
    {
        return;
    }
    impl_->metadata = *loaded;
    impl_->valid = true;

    // Checked up front so a caller sees the whole story before reading rather
    // than discovering it partway through. A missing or incomplete part is not
    // fatal -- see the header.
    for (const bag_part_t& part : impl_->metadata.parts)
    {
        const std::filesystem::path path = std::filesystem::path(impl_->directory) / part.path;

        std::error_code error;
        if (!std::filesystem::exists(path, error))
        {
            impl_->problems.push_back("part '" + part.path +
                                      "' is listed in metadata.yaml but not on disk");
            continue;
        }

        if (!part.complete)
        {
            impl_->problems.push_back(
                "part '" + part.path +
                "' has no summary -- its writer died. Messages up to the last complete chunk "
                "are still readable; `bag reindex` can rebuild the summary.");
        }
    }

    if (impl_->metadata.dropped_messages > 0)
    {
        impl_->problems.push_back(
            std::to_string(impl_->metadata.dropped_messages) +
            " message(s) were dropped during recording -- the recorder could not keep up. Gaps "
            "in this bag are not necessarily gaps in what was published.");
    }
}

BagReader::~BagReader() = default;

bool BagReader::isValid() const
{
    return impl_->valid;
}

const bag_metadata_t& BagReader::metadata() const
{
    return impl_->metadata;
}

const std::vector<std::string>& BagReader::problems() const
{
    return impl_->problems;
}

std::span<const std::uint8_t> BagReader::descriptorFor(std::string_view schema_name) const
{
    impl_->loadDescriptors();

    const auto found = impl_->descriptors.find(std::string(schema_name));
    if (found == impl_->descriptors.end())
    {
        return {};
    }
    return found->second;
}

bool BagReader::forEach(std::uint64_t start_ns, std::uint64_t end_ns,
                        const std::function<bool(const BagMessage&)>& callback)
{
    if (!impl_->valid)
    {
        return false;
    }

    for (const bag_part_t& part : impl_->metadata.parts)
    {
        // Parts do not overlap in time -- the writer rolls, it does not
        // interleave -- so visiting them in order gives a globally ordered
        // stream, and one entirely outside the window can be skipped without
        // opening the file at all. On a multi-gigabyte recording that is the
        // difference between a seek and a scan.
        if (part.message_count > 0 && (part.t_end_ns < start_ns || part.t_begin_ns > end_ns))
        {
            continue;
        }

        const std::string path = (std::filesystem::path(impl_->directory) / part.path).string();

        std::error_code exists_error;
        if (!std::filesystem::exists(path, exists_error))
        {
            continue;  // Already reported by the constructor.
        }

        mcap::McapReader reader;
        const mcap::Status status = reader.open(path);
        if (!status.ok())
        {
            SPDLOG_ERROR("Could not open '{}': {}", path, status.message);
            return false;
        }

        // Build the seeking indexes, falling back to a sequential scan when the
        // summary is missing or incomplete.
        //
        // THIS LINE IS WHY A CRASHED RECORDING IS READABLE. A part whose writer
        // was killed has no summary -- no ChunkIndex, no Statistics, no footer.
        // Without an explicit fallback the reader finds no index, and
        // readMessages() in LogTimeOrder (which needs chunk indexes to merge
        // channels) yields NOTHING. Not an error, not a warning: zero messages
        // from a file with megabytes of perfectly good data in it.
        //
        // That is the worst possible failure for this tool. The recording you
        // most want is the one that ended in a crash, and it would have come
        // back empty while reporting success.
        //
        // The scan costs a pass over the data section, paid only for a part that
        // actually lacks a summary -- AllowFallbackScan uses the summary when
        // there is one.
        const mcap::Status summary = reader.readSummary(
            mcap::ReadSummaryMethod::AllowFallbackScan,
            [&](const mcap::Status& problem)
            { SPDLOG_DEBUG("'{}' while indexing: {}", path, problem.message); });

        mcap::ReadMessageOptions options;
        options.startTime = start_ns;

        // MCAP's range is half-open at the top and ours is closed, because a
        // caller asking for "up to t" means "including anything stamped exactly
        // t". Guarded against overflow: the default end is UINT64_MAX.
        options.endTime = end_ns == std::numeric_limits<std::uint64_t>::max() ? end_ns
                                                                             : end_ns + 1;

        // Sorted by log time WHEN THERE IS A MESSAGE INDEX TO SORT BY, and
        // sequentially otherwise.
        //
        // The condition is not `summary.ok()`, and that distinction is the whole
        // fix. AllowFallbackScan on a torn part SUCCEEDS: it scans the data
        // section and produces perfectly good ChunkIndex records. What it cannot
        // produce is MessageIndex records, because those live in the summary
        // that the crash took with it -- and LogTimeOrder needs them to merge
        // channels. Asked for it anyway, the reader reports "cannot read MCAP in
        // time order with no message indexes" through the problem callback and
        // yields ZERO messages.
        //
        // So a status check passes, the read succeeds, and a file with megabytes
        // of recoverable data comes back empty. That is the worst failure this
        // tool could have: the recording you most want is the one that ended in
        // a crash.
        //
        // The tradeoff of falling back is real but small: file order is only
        // APPROXIMATELY time order once several channels interleave within a
        // chunk. An intact recording gets exact ordering; a damaged one gets its
        // messages at all, slightly out of order.
        bool has_message_indexes = false;
        for (const mcap::ChunkIndex& chunk : reader.chunkIndexes())
        {
            if (!chunk.messageIndexOffsets.empty())
            {
                has_message_indexes = true;
                break;
            }
        }

        options.readOrder = has_message_indexes
                                ? mcap::ReadMessageOptions::ReadOrder::LogTimeOrder
                                : mcap::ReadMessageOptions::ReadOrder::FileOrder;

        if (!has_message_indexes)
        {
            SPDLOG_WARN("'{}' has no message index -- its writer did not finish{}. Reading it "
                        "sequentially; messages may be slightly out of time order.",
                        path, summary.ok() ? "" : (" (" + summary.message + ")"));
        }

        // The problem list already told the caller about a torn part; here we
        // just take what is readable and move on rather than aborting the whole
        // recording for the last few damaged bytes.
        auto onProblem = [&](const mcap::Status& problem)
        {
            SPDLOG_DEBUG("'{}': {}", path, problem.message);
        };

        bool keep_going = true;
        for (const mcap::MessageView& view : reader.readMessages(onProblem, options))
        {
            BagMessage message;
            message.key = view.channel->topic;

            // The registry name from the channel metadata, not the Schema
            // record's name -- that one is capnp's qualified form, which is
            // right for a foreign consumer and wrong for us.
            if (const auto found = view.channel->metadata.find("redline/schema");
                found != view.channel->metadata.end())
            {
                message.schema = found->second;
            }
            else if (view.schema)
            {
                message.schema = view.schema->name;
            }

            message.payload = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(view.message.data),
                view.message.dataSize);
            message.log_time_ns = view.message.logTime;
            message.publish_time_ns = view.message.publishTime;

            if (!callback(message))
            {
                keep_going = false;
                break;
            }
        }

        reader.close();

        if (!keep_going)
        {
            break;
        }
    }

    return true;
}

}  // namespace bag
