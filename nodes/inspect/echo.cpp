#include "inspect/verbs.h"

#include "cli/interrupt.h"
#include "cli/output.h"

#include "pub_sub/capnp_json.h"
#include "pub_sub/capnp_payload.h"
#include "pub_sub/raw_subscriber.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/timestamp.h"

#include <capnp/dynamic.h>
#include <capnp/pretty-print.h>
#include <capnp/serialize.h>

#include <spdlog/fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace inspect
{

namespace
{

// A publish timestamp as local wall-clock time, to millisecond resolution.
//
// Formatted rather than printed raw because the raw value is nanoseconds since
// 1970: a nineteen-digit number that nobody can read, and whose most common
// failure mode -- being wrong by a factor of 2^32 -- is invisible until you see
// it next to a date.
std::string formatTimestamp(std::uint64_t unix_nanos)
{
    const auto seconds = static_cast<std::time_t>(unix_nanos / 1'000'000'000ull);
    const auto millis = static_cast<unsigned>((unix_nanos % 1'000'000'000ull) / 1'000'000ull);

    std::tm parts{};
    ::localtime_r(&seconds, &parts);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &parts);

    return fmt::format("{}.{:03}", buffer, millis);
}

}  // namespace

void addEchoOptions(cxxopts::Options& options)
{
    options.add_options()
        ("k,key", "Key expression to subscribe to. Wildcards allowed.",
            cxxopts::value<std::string>())
        ("n,count", "Exit after this many messages.", cxxopts::value<std::uint64_t>())
        ("t,timestamps", "Show each message's publish time and origin session.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("hex", "Always hex-dump, never decode.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"));

    // So `inspect echo vehicle/engine/rpm` works. docs/carplay_bringup.md has
    // documented that form for a long time; no verb actually accepted it, and
    // the key was silently dropped into unmatched().
    options.parse_positional({"key"});
}

int runEcho(cli::Context& context)
{
    const auto key = context.requireString("key");
    if (!key)
    {
        return cli::kUsage;
    }

    const bool as_json = context.json();
    const bool show_timestamps = context.flag("timestamps");
    const bool force_hex = context.flag("hex");
    const std::uint64_t limit = context.uintOr("count", 0);

    std::atomic<std::uint64_t> received{0};
    std::mutex print_mutex;

    pub_sub::RawSubscriber subscriber(
        *key,
        pub_sub::RawSubscriber::InfoHandler(
            [&](const std::vector<std::uint8_t>& payload,
                const pub_sub::RawSubscriber::SampleInfo& info)
            {
                // One message printed at a time: a wildcard subscription can
                // deliver from several keys concurrently on different zenoh
                // threads, and interleaved half-lines are unreadable.
                const std::lock_guard<std::mutex> guard(print_mutex);

                std::string prefix;
                if (show_timestamps)
                {
                    prefix = info.publish_time_nanos
                                 ? fmt::format("[{} {}] ",
                                               formatTimestamp(*info.publish_time_nanos),
                                               info.origin_zid)
                                 : std::string("[unstamped] ");
                }

                // The concrete key, which matters whenever the subscription was
                // a wildcard.
                const std::string label = fmt::format("{}{}", prefix, info.keyexpr);

                const auto schema = pub_sub::get_schema(info.schema_name);
                const pub_sub::WordAlignedPayload aligned(payload);

                if (force_hex || !schema || aligned.empty())
                {
                    if (!force_hex && !schema)
                    {
                        SPDLOG_WARN("Schema '{}' is not in this build's registry; hex-dumping.",
                                    info.schema_name);
                    }
                    else if (!force_hex && aligned.empty())
                    {
                        // Not a whole number of capnp words, so there is no
                        // message here. Decoding the truncated remainder would
                        // print a plausible struct full of defaults -- which
                        // reads exactly like a valid message reporting zeroes.
                        SPDLOG_WARN("{}: {} bytes is not a whole number of {}-byte capnp words; "
                                    "hex-dumping.",
                                    info.keyexpr, payload.size(), sizeof(capnp::word));
                    }
                    cli::out("{} {} bytes: [{:02X}]", label, payload.size(),
                             fmt::join(payload, " "));
                }
                else if (as_json)
                {
                    try
                    {
                        const nlohmann::json decoded = pub_sub::capnpToJson(payload, *schema);
                        nlohmann::json row;
                        row["key"] = std::string(info.keyexpr);
                        row["schema"] = std::string(info.schema_name);
                        if (info.publish_time_nanos)
                        {
                            row["publish_time_ns"] = *info.publish_time_nanos;
                            row["origin_zid"] = std::string(info.origin_zid);
                        }
                        row["message"] = decoded;
                        cli::out("{}", row.dump());
                        cli::flush();
                    }
                    catch (const kj::Exception& e)
                    {
                        SPDLOG_ERROR("{}: could not decode: {}", info.keyexpr,
                                     e.getDescription().cStr());
                    }
                }
                else
                {
                    capnp::FlatArrayMessageReader reader(aligned.words());
                    auto root = reader.getRoot<capnp::DynamicStruct>(schema->asStruct());
                    cli::out("{} {}", label, capnp::prettyPrint(root).flatten().cStr());
                    cli::flush();
                }

                ++received;
            }));

    if (!subscriber.isValid())
    {
        SPDLOG_ERROR("Could not subscribe to '{}'.", *key);
        return cli::kFailure;
    }

    cli::installInterruptHandler();

    if (limit == 0)
    {
        SPDLOG_INFO("Subscribed to '{}'. Ctrl-C to stop.", *key);
    }

    while (!cli::interrupted())
    {
        if (limit != 0 && received.load(std::memory_order_relaxed) >= limit)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // A count that was asked for and not reached is a failure: a script running
    // `inspect echo -n 10` wants ten messages, and getting three because someone
    // pressed Ctrl-C is not success.
    if (limit != 0 && received.load(std::memory_order_relaxed) < limit)
    {
        SPDLOG_WARN("Interrupted after {} of {} requested messages.", received.load(), limit);
        return cli::kFailure;
    }

    return cli::kOk;
}

}  // namespace inspect
