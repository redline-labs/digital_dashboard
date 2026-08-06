#include "bag/writer.h"

#include "bag/validate.h"

#include "pub_sub/schema_registry.h"

#include <mcap/writer.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <system_error>

namespace bag
{

namespace
{

mcap::Compression codecFromName(const std::string& name)
{
    if (name == "none")
    {
        return mcap::Compression::None;
    }
    if (name == "lz4")
    {
        return mcap::Compression::Lz4;
    }
    return mcap::Compression::Zstd;
}

mcap::CompressionLevel levelFromInt(int level)
{
    // mcap's levels are named rather than numeric, so this maps a familiar
    // 0-9 scale onto them. Zero means "the codec's default", which for zstd is
    // level 3 -- fast enough for a full bus.
    switch (level)
    {
        case 1:
        case 2:
            return mcap::CompressionLevel::Fastest;
        case 3:
        case 4:
            return mcap::CompressionLevel::Fast;
        case 5:
        case 6:
            return mcap::CompressionLevel::Default;
        case 7:
        case 8:
            return mcap::CompressionLevel::Slow;
        case 9:
            return mcap::CompressionLevel::Slowest;
        default:
            return mcap::CompressionLevel::Default;
    }
}

std::string isoNow()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);

    std::tm parts{};
    ::localtime_r(&seconds, &parts);

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S%z", &parts);
    return buffer;
}

}  // namespace

struct BagWriter::Impl
{
    std::string directory;
    WriterOptions options;
    bag_metadata_t metadata;

    bool valid = false;
    bool closed = false;

    // A FRESH WRITER PER PART, never a reused one.
    //
    // mcap::McapWriter deliberately retains its schemas and channels across
    // close()/open() -- its documentation offers that as an efficiency for
    // writing several files. It is a trap here. On a roll the writer keeps the
    // previous part's registrations, assigns NEW ids to the ones we re-add, and
    // writes BOTH sets into the new file's summary -- so the summary references
    // schema and channel ids that its own data section never contains.
    //
    // Our own reader did not care: it reads channels through the message views
    // and found everything. `bag info` reported correct counts. The file was
    // still malformed, and `mcap doctor` said so:
    //
    //     Schema with id 1 in summary section does not exist in data section
    //     Encountered Channel (1) with unknown Schema (1)
    //
    // which is exactly the class of bug that ships as "works for us, broken for
    // everyone else". Constructing a new writer makes the reset total.
    std::unique_ptr<mcap::McapWriter> writer;
    std::string part_path;
    std::size_t part_index = 0;
    std::uint64_t part_messages = 0;
    std::uint64_t part_t_begin = 0;
    std::uint64_t part_t_end = 0;
    std::chrono::steady_clock::time_point part_started;

    // MCAP ids are per FILE, so both of these are cleared on every roll. Reusing
    // an id across parts would produce a second part whose channels point at
    // schemas it never declared.
    std::map<std::string, mcap::ChannelId> channels;
    std::map<std::string, mcap::SchemaId> schemas;

    // Accumulated across the whole recording, keyed by topic.
    struct TopicState
    {
        std::string schema;
        std::uint64_t count = 0;
        std::string origin_zid;
        bool origin_conflict = false;
        bool ever_published = false;
    };
    std::map<std::string, TopicState> topics;

    // Checked periodically rather than after every message: file_size() is a
    // syscall, and mcap buffers a chunk before writing anyway, so the size lags
    // regardless. Rolling is therefore approximate and always slightly late,
    // which is the right direction to be wrong in -- a part a little over the
    // limit is harmless; one cut short is not.
    static constexpr std::uint64_t kSizeCheckInterval = 256;

    std::string partName(std::size_t index) const
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "_%04zu.mcap", index);
        return options.name + buffer;
    }

    bool openPart()
    {
        const std::string name = partName(part_index);
        part_path = (std::filesystem::path(directory) / name).string();

        mcap::McapWriterOptions writer_options("");
        writer_options.compression = codecFromName(options.compression);
        writer_options.compressionLevel = levelFromInt(options.compression_level);
        writer_options.chunkSize = options.chunk_bytes;
        writer_options.library = options.recorder;

        // noChunking would make the file unseekable: without chunks there are no
        // ChunkIndex records, and `bag play --start-offset` degrades from a seek
        // into a full scan. Even with compression "none" the chunking stays.
        writer_options.noChunking = false;

        writer = std::make_unique<mcap::McapWriter>();
        const mcap::Status status = writer->open(part_path, writer_options);
        if (!status.ok())
        {
            SPDLOG_ERROR("Could not open '{}': {}", part_path, status.message);
            return false;
        }

        channels.clear();
        schemas.clear();
        part_messages = 0;
        part_t_begin = 0;
        part_t_end = 0;
        part_started = std::chrono::steady_clock::now();
        return true;
    }

    void finishPart()
    {
        if (writer)
        {
            writer->close();
        }

        bag_part_t part;
        part.path = partName(part_index);
        part.message_count = part_messages;
        part.t_begin_ns = part_t_begin;
        part.t_end_ns = part_t_end;

        // Verified rather than assumed. close() having returned is not proof
        // the file landed on disk intact -- a full disk, a failing device or an
        // I/O error partway through the footer all produce a part that looks
        // closed from in here. Reading the trailing magic back costs 8 bytes and
        // makes `complete` mean what a reader thinks it means.
        part.complete = hasCompleteEnding(part_path);

        std::error_code error;
        part.bytes = std::filesystem::file_size(part_path, error);
        if (error)
        {
            part.bytes = 0;
        }

        metadata.parts.push_back(std::move(part));
    }

    bool roll()
    {
        finishPart();
        ++part_index;
        return openPart();
    }

    bool shouldRoll()
    {
        if (options.max_part_seconds > 0.0)
        {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - part_started)
                    .count();
            if (elapsed >= options.max_part_seconds)
            {
                return true;
            }
        }

        if (options.max_part_bytes > 0 && (part_messages % kSizeCheckInterval) == 0)
        {
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(part_path, error);
            if (!error && size >= options.max_part_bytes)
            {
                return true;
            }
        }

        return false;
    }

    // Registers the schema in the CURRENT part if it is not already there.
    mcap::SchemaId schemaIdFor(std::string_view schema_name)
    {
        const std::string name(schema_name);
        if (const auto found = schemas.find(name); found != schemas.end())
        {
            return found->second;
        }

        // The descriptor is the schema as DATA -- a serialized capnp
        // CodeGeneratorRequest, pruned to this schema's transitive closure. It
        // is what makes a recording decodable by something that does not link
        // our generated headers, which is the whole reason a bag is worth more
        // than a hex dump.
        //
        // Empty for a schema this build does not know, which is a real case: a
        // node from a newer build publishing a type we have never heard of. The
        // messages are still recorded byte-for-byte and the channel still names
        // the schema; only the means to decode them is missing, and that is
        // better than refusing to record them at all.
        const std::span<const std::uint8_t> descriptor = pub_sub::schema_descriptor(schema_name);
        if (descriptor.empty() && !name.empty())
        {
            SPDLOG_WARN("Schema '{}' is not in this build's registry, so '{}' will carry no "
                        "schema definition. The messages are still recorded verbatim.",
                        name, part_path);
        }

        mcap::Schema schema;

        // capnp's own qualified name, not our registry name. A consumer reading
        // the descriptor resolves the root node by this; the registry name
        // appears nowhere in the node graph, so naming the record after it would
        // give a reader a name it cannot look up.
        //
        // The registry name is not lost -- it goes on the channel below, which
        // is where our own tools read it from.
        std::string display_name = name;
        if (const auto schema_type =
                reflection::enum_traits<pub_sub::schema_type_t>::try_from_string(schema_name))
        {
            display_name = std::string(pub_sub::schema_display_name(*schema_type));
        }

        schema.name = display_name;
        schema.encoding = descriptor.empty() ? "" : "capnproto";
        schema.data.assign(reinterpret_cast<const std::byte*>(descriptor.data()),
                           reinterpret_cast<const std::byte*>(descriptor.data() +
                                                              descriptor.size()));

        writer->addSchema(schema);
        schemas.emplace(name, schema.id);
        return schema.id;
    }

    mcap::ChannelId channelIdFor(std::string_view key, std::string_view schema_name)
    {
        const std::string topic(key);
        if (const auto found = channels.find(topic); found != channels.end())
        {
            return found->second;
        }

        mcap::Channel channel;
        channel.topic = topic;
        channel.messageEncoding = "capnproto";
        channel.schemaId = schemaIdFor(schema_name);

        // Our registry name, which is what `inspect`, the dashboard configs and
        // pub_sub::get_schema() all speak. The Schema record carries capnp's
        // qualified name for foreign consumers; this is for ours.
        channel.metadata["redline/schema"] = std::string(schema_name);

        writer->addChannel(channel);
        channels.emplace(topic, channel.id);
        return channel.id;
    }
};

BagWriter::BagWriter(std::string directory, WriterOptions options) :
    impl_(std::make_unique<Impl>())
{
    impl_->directory = std::move(directory);
    impl_->options = std::move(options);

    if (impl_->options.name.empty())
    {
        // The directory's own name, so `bag record drives/2026-08-06` produces
        // `2026-08-06_0000.mcap` rather than something anonymous.
        impl_->options.name = std::filesystem::path(impl_->directory).filename().string();
        if (impl_->options.name.empty())
        {
            impl_->options.name = "bag";
        }
    }

    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error)
    {
        SPDLOG_ERROR("Could not create '{}': {}", impl_->directory, error.message());
        return;
    }

    impl_->metadata.version = 1;
    impl_->metadata.created = isoNow();
    impl_->metadata.recorder = impl_->options.recorder;
    impl_->metadata.compression = impl_->options.compression;

    impl_->valid = impl_->openPart();
}

BagWriter::~BagWriter()
{
    if (!impl_->closed)
    {
        (void)close();
    }
}

bool BagWriter::isValid() const
{
    return impl_->valid;
}

bool BagWriter::write(std::string_view key, std::string_view schema_name,
                      std::span<const std::uint8_t> payload, std::uint64_t log_time_ns,
                      std::optional<std::uint64_t> publish_time_ns, std::string_view origin_zid)
{
    if (!impl_->valid || impl_->closed)
    {
        return false;
    }

    if (impl_->shouldRoll() && !impl_->roll())
    {
        impl_->valid = false;
        return false;
    }

    mcap::Message message;
    message.channelId = impl_->channelIdFor(key, schema_name);
    message.sequence = 0;
    message.logTime = log_time_ns;

    // Falls back to log_time, and SAYS SO by counting. MCAP's own guidance is to
    // set publishTime to logTime when no publish time is available; the count is
    // ours, because otherwise a bag full of synthesised publish times is
    // indistinguishable from one with real ones, and anything measuring latency
    // from it would be measuring zero.
    if (publish_time_ns)
    {
        message.publishTime = *publish_time_ns;
    }
    else
    {
        message.publishTime = log_time_ns;
        ++impl_->metadata.unstamped_messages;
    }

    message.dataSize = payload.size();
    message.data = reinterpret_cast<const std::byte*>(payload.data());

    const mcap::Status status = impl_->writer->write(message);
    if (!status.ok())
    {
        SPDLOG_ERROR("Write to '{}' failed: {}", impl_->part_path, status.message);
        impl_->valid = false;
        return false;
    }

    ++impl_->part_messages;
    ++impl_->metadata.message_count;

    if (impl_->part_t_begin == 0 || log_time_ns < impl_->part_t_begin)
    {
        impl_->part_t_begin = log_time_ns;
    }
    impl_->part_t_end = std::max(impl_->part_t_end, log_time_ns);

    if (impl_->metadata.t_begin_ns == 0 || log_time_ns < impl_->metadata.t_begin_ns)
    {
        impl_->metadata.t_begin_ns = log_time_ns;
    }
    impl_->metadata.t_end_ns = std::max(impl_->metadata.t_end_ns, log_time_ns);

    Impl::TopicState& topic = impl_->topics[std::string(key)];
    topic.schema = std::string(schema_name);
    ++topic.count;
    topic.ever_published = true;

    if (!origin_zid.empty())
    {
        if (topic.origin_zid.empty())
        {
            topic.origin_zid = std::string(origin_zid);
        }
        else if (topic.origin_zid != origin_zid)
        {
            // Two sessions publishing the same key. Recorded rather than
            // averaged away: it means two nodes are fighting over a topic, and
            // that is a genuine misconfiguration worth surfacing afterwards.
            topic.origin_conflict = true;
        }
    }

    return true;
}

void BagWriter::noteAdvertised(std::string_view key, std::string_view schema_name)
{
    Impl::TopicState& topic = impl_->topics[std::string(key)];
    if (topic.schema.empty())
    {
        topic.schema = std::string(schema_name);
    }
}

void BagWriter::noteDropped(std::uint64_t count)
{
    impl_->metadata.dropped_messages += count;
}

bool BagWriter::close()
{
    if (impl_->closed)
    {
        return true;
    }
    impl_->closed = true;

    if (!impl_->valid)
    {
        // Still write what we have. A recording that failed partway is more
        // useful with an index than without one.
        (void)saveMetadata(impl_->metadata, impl_->directory);
        return false;
    }

    impl_->finishPart();

    impl_->metadata.topics.clear();
    for (const auto& [key, state] : impl_->topics)
    {
        bag_topic_t topic;
        topic.key = key;
        topic.schema = state.schema;
        topic.message_count = state.count;
        topic.origin_zid = state.origin_conflict ? "(mixed)" : state.origin_zid;
        topic.advertised_only = !state.ever_published;
        impl_->metadata.topics.push_back(std::move(topic));
    }

    return saveMetadata(impl_->metadata, impl_->directory);
}

const bag_metadata_t& BagWriter::metadata() const
{
    return impl_->metadata;
}

}  // namespace bag
