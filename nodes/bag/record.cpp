#include "bag_tool/verbs.h"

#include "bag/queue.h"
#include "bag/writer.h"

#include "cli/interrupt.h"
#include "cli/output.h"

#include "pub_sub/node_identity.h"
#include "pub_sub/raw_subscriber.h"
#include "pub_sub/topic_directory.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace bag_tool
{

namespace
{

std::uint64_t wallClockNanos()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

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

}  // namespace

void addRecordOptions(cxxopts::Options& options)
{
    options.add_options()
        ("output", "Directory to write the recording into.", cxxopts::value<std::string>())
        ("k,key", "Key expression to record. Repeatable. Defaults to everything.",
            cxxopts::value<std::vector<std::string>>())
        ("compression", "Chunk codec: none, lz4 or zstd.",
            cxxopts::value<std::string>()->default_value("zstd"))
        ("compression-level", "1 (fastest) to 9 (smallest); 0 for the codec's default.",
            cxxopts::value<int>()->default_value("0"))
        ("chunk-size", "Uncompressed bytes buffered before a chunk is flushed.",
            cxxopts::value<std::uint64_t>()->default_value("4194304"))
        ("max-size", "Roll to a new part past this many bytes. 0 disables.",
            cxxopts::value<std::uint64_t>()->default_value("2147483648"))
        ("max-duration", "Roll to a new part past this many seconds. 0 disables.",
            cxxopts::value<double>()->default_value("0"))
        ("queue-depth", "Messages buffered between the bus and the writer thread.",
            cxxopts::value<std::uint64_t>()->default_value("8192"))
        ("d,duration", "Stop after this many seconds. 0 means until Ctrl-C.",
            cxxopts::value<double>()->default_value("0"))
        ("quiet", "Do not print the progress line.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"));

    options.parse_positional({"output"});
}

int runRecord(cli::Context& context)
{
    const auto output = context.requireString("output");
    if (!output)
    {
        return cli::kUsage;
    }

    std::vector<std::string> keys;
    if (context.has("key"))
    {
        keys = context.args()["key"].as<std::vector<std::string>>();
    }
    if (keys.empty())
    {
        keys.push_back("**");
    }

    bag::WriterOptions writer_options;
    writer_options.compression = context.stringOr("compression", "zstd");
    writer_options.compression_level = static_cast<int>(context.uintOr("compression-level", 0));
    writer_options.chunk_bytes = context.uintOr("chunk-size", 4ull * 1024 * 1024);
    writer_options.max_part_bytes = context.uintOr("max-size", 2ull * 1024 * 1024 * 1024);
    writer_options.max_part_seconds = context.doubleOr("max-duration", 0.0);
    writer_options.recorder = "redline bag";

    if (writer_options.compression != "none" && writer_options.compression != "lz4" &&
        writer_options.compression != "zstd")
    {
        SPDLOG_ERROR("--compression must be none, lz4 or zstd.");
        return cli::kUsage;
    }

    bag::BagWriter writer(*output, writer_options);
    if (!writer.isValid())
    {
        return cli::kFailure;
    }

    // So the recorder itself is visible on the bus while it runs. A long capture
    // is a process somebody may need to find.
    pub_sub::NodeIdentity identity("bag_record");

    // What EXISTS, alongside what flows. A topic advertised for the whole
    // recording and never published is a fact only liveliness can supply, and
    // one a file full of messages can never express afterwards -- "produced
    // nothing" and "was not running" look identical once the bus is gone.
    pub_sub::TopicDirectory directory;

    bag::MessageQueue queue(context.uintOr("queue-depth", 8192));

    // The writer runs on its own thread. The zenoh callbacks below must not
    // block -- they run on zenoh RX threads, and stalling one stalls the session
    // for everything including the liveliness traffic other tools depend on -- so
    // they copy into the queue and return immediately.
    std::atomic<bool> writer_failed{false};
    std::thread writer_thread(
        [&]
        {
            while (const auto message = queue.pop())
            {
                if (!writer.write(message->key, message->schema, message->payload,
                                  message->log_time_ns, message->publish_time_ns,
                                  message->origin_zid))
                {
                    writer_failed = true;
                    break;
                }
            }
        });

    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> bytes{0};

    std::vector<std::unique_ptr<pub_sub::RawSubscriber>> subscribers;
    for (const std::string& key : keys)
    {
        auto subscriber = std::make_unique<pub_sub::RawSubscriber>(
            key, pub_sub::RawSubscriber::InfoHandler(
                     [&](const std::vector<std::uint8_t>& payload,
                         const pub_sub::RawSubscriber::SampleInfo& info)
                     {
                         // Arrival time, taken here rather than on the writer
                         // thread: a queue that is backing up would otherwise
                         // fold its own latency into every log_time, and the
                         // recording's timing would slew under load.
                         bag::QueuedMessage message;
                         message.log_time_ns = wallClockNanos();
                         message.key = std::string(info.keyexpr);
                         message.schema = std::string(info.schema_name);
                         message.origin_zid = std::string(info.origin_zid);
                         message.publish_time_ns = info.publish_time_nanos;
                         message.payload = payload;

                         bytes += payload.size();
                         ++received;
                         queue.push(std::move(message));
                     }));

        if (!subscriber->isValid())
        {
            SPDLOG_ERROR("Could not subscribe to '{}'.", key);
            queue.stop();
            writer_thread.join();
            return cli::kFailure;
        }
        subscribers.push_back(std::move(subscriber));
    }

    cli::installInterruptHandler();

    const double duration = context.doubleOr("duration", 0.0);
    const bool quiet = context.flag("quiet") || context.json();
    const auto started = std::chrono::steady_clock::now();

    cli::out("Recording {} into '{}' ({}). Ctrl-C to stop.",
             keys.size() == 1 ? keys.front() : fmt::format("{} key expressions", keys.size()),
             *output, writer_options.compression);

    std::uint64_t last_received = 0;
    while (!cli::interrupted() && !writer_failed)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        if (duration > 0.0 && elapsed >= duration)
        {
            break;
        }

        // Snapshot the advertisement set as it changes, so a node that starts
        // and stops during the recording is still recorded as having existed.
        for (const pub_sub::DirectoryEntry& entry : directory.snapshot())
        {
            writer.noteAdvertised(entry.key, entry.schema);
        }

        if (!quiet)
        {
            const std::uint64_t total = received.load();
            const double rate = static_cast<double>(total - last_received) / 0.5;
            last_received = total;

            cli::outPartial("\r{:>8.0f}s  {:>9} msgs  {:>7.0f}/s  {:>10}  queue {:>5}  drops {}",
                            elapsed, total, rate, humanBytes(static_cast<double>(bytes.load())),
                            queue.depth(), queue.dropped());
            cli::flush();
        }
    }

    if (!quiet)
    {
        cli::out("");
    }

    // Order matters: stop the subscribers FIRST so nothing new is queued, then
    // drain. Stopping the queue first would discard whatever the callbacks were
    // still adding.
    subscribers.clear();
    queue.stop();
    writer_thread.join();

    writer.noteDropped(queue.dropped());

    const bool closed = writer.close();

    const bag::bag_metadata_t& metadata = writer.metadata();

    if (context.json())
    {
        nlohmann::json summary;
        summary["output"] = *output;
        summary["messages"] = metadata.message_count;
        summary["dropped"] = metadata.dropped_messages;
        summary["unstamped"] = metadata.unstamped_messages;
        summary["parts"] = metadata.parts.size();
        cli::out("{}", summary.dump(2));
    }
    else
    {
        cli::out("Wrote {} message(s) in {} part(s) to '{}'.", metadata.message_count,
                 metadata.parts.size(), *output);

        if (metadata.dropped_messages > 0)
        {
            // Loud, because a silently lossy recording is worse than none: a gap
            // in a trace reads as a publisher that stopped, which is a
            // completely different fault to chase.
            SPDLOG_WARN("{} message(s) were DROPPED -- the recorder could not keep up. Try a "
                        "larger --queue-depth, --compression lz4, or a faster disk.",
                        metadata.dropped_messages);
        }

        if (metadata.unstamped_messages > 0)
        {
            SPDLOG_WARN("{} message(s) arrived without a publish timestamp; their publish_time "
                        "was taken from arrival. `bag info` reports this too.",
                        metadata.unstamped_messages);
        }
    }

    if (writer_failed || !closed)
    {
        return cli::kFailure;
    }

    return cli::kOk;
}

}  // namespace bag_tool
