#include "bag_tool/verbs.h"

#include "bag/reader.h"

#include "cli/interrupt.h"
#include "cli/output.h"

#include "pub_sub/detail/byte_publisher.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/topic_key.h"

#include <capnp/common.h>

#include <kj/array.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace bag_tool
{

namespace
{

// old=new, repeatable.
std::map<std::string, std::string> parseRemaps(const std::vector<std::string>& raw)
{
    std::map<std::string, std::string> remaps;
    for (const std::string& entry : raw)
    {
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos)
        {
            SPDLOG_ERROR("--remap wants old=new, got '{}'.", entry);
            continue;
        }
        remaps.emplace(entry.substr(0, equals), entry.substr(equals + 1));
    }
    return remaps;
}

}  // namespace

void addPlayOptions(cxxopts::Options& options)
{
    options.add_options()
        ("bag", "The recording directory.", cxxopts::value<std::string>())
        ("r,rate", "Playback speed multiplier. 0 means as fast as possible.",
            cxxopts::value<double>()->default_value("1.0"))
        ("l,loop", "Replay from the start when the recording ends.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("s,start-offset", "Begin this many seconds into the recording.",
            cxxopts::value<double>()->default_value("0"))
        ("d,duration", "Play only this many seconds. 0 means to the end.",
            cxxopts::value<double>()->default_value("0"))
        ("k,key", "Only replay these keys. Repeatable. Defaults to everything.",
            cxxopts::value<std::vector<std::string>>())
        ("remap", "Republish 'old' as 'new'. Repeatable.",
            cxxopts::value<std::vector<std::string>>())
        ("prefix", "Prepend this to every key, so a replay does not collide with live nodes.",
            cxxopts::value<std::string>());

    options.parse_positional({"bag"});
}

int runPlay(cli::Context& context)
{
    const auto path = context.requireString("bag");
    if (!path)
    {
        return cli::kUsage;
    }

    bag::BagReader reader(*path);
    if (!reader.isValid())
    {
        return cli::kFailure;
    }

    for (const std::string& problem : reader.problems())
    {
        SPDLOG_WARN("{}", problem);
    }

    const bag::bag_metadata_t& metadata = reader.metadata();
    if (metadata.message_count == 0)
    {
        SPDLOG_ERROR("'{}' contains no messages.", *path);
        return cli::kFailure;
    }

    const double rate = context.doubleOr("rate", 1.0);
    if (rate < 0.0)
    {
        SPDLOG_ERROR("--rate must not be negative.");
        return cli::kUsage;
    }

    const std::string prefix = context.stringOr("prefix", "");
    const std::map<std::string, std::string> remaps =
        parseRemaps(context.has("remap") ? context.args()["remap"].as<std::vector<std::string>>()
                                         : std::vector<std::string>{});

    std::vector<std::string> only_keys;
    if (context.has("key"))
    {
        only_keys = context.args()["key"].as<std::vector<std::string>>();
    }

    const std::uint64_t start_ns =
        metadata.t_begin_ns +
        static_cast<std::uint64_t>(context.doubleOr("start-offset", 0.0) * 1e9);
    const double play_duration = context.doubleOr("duration", 0.0);
    const std::uint64_t end_ns =
        play_duration > 0.0 ? start_ns + static_cast<std::uint64_t>(play_duration * 1e9)
                            : std::numeric_limits<std::uint64_t>::max();

    pub_sub::NodeIdentity identity("bag_play");

    // One publisher per channel, created lazily.
    //
    // detail::BytePublisher rather than a raw zenoh put, because it also
    // declares the topic's liveliness advertisement -- so scope's picker and
    // `inspect list` see a replayed topic exactly as they see a live one, with
    // its schema and its owning session. A replay that published samples but
    // advertised nothing would be invisible to every discovery-based tool, which
    // is most of them now.
    std::map<std::string, std::unique_ptr<pub_sub::detail::BytePublisher>> publishers;

    const auto resolveKey = [&](std::string_view key) -> std::string
    {
        std::string out(key);
        if (const auto found = remaps.find(out); found != remaps.end())
        {
            out = found->second;
        }
        if (!prefix.empty())
        {
            out = prefix + "/" + out;
        }
        return out;
    };

    cli::installInterruptHandler();

    std::uint64_t published = 0;
    std::uint64_t skipped_unaligned = 0;
    std::uint64_t skipped_bad_key = 0;

    const bool loop = context.flag("loop");

    do
    {
        // Wall time and recording time are pinned together at the first message
        // of each pass, so timing does not drift across a loop.
        std::optional<std::uint64_t> first_log_time;
        std::chrono::steady_clock::time_point wall_start;

        const bool ok = reader.forEach(
            start_ns, end_ns,
            [&](const bag::BagMessage& message) -> bool
            {
                if (cli::interrupted())
                {
                    return false;
                }

                if (!only_keys.empty() &&
                    std::find(only_keys.begin(), only_keys.end(), message.key) ==
                        only_keys.end())
                {
                    return true;
                }

                if (!first_log_time)
                {
                    first_log_time = message.log_time_ns;
                    wall_start = std::chrono::steady_clock::now();
                }

                // Sleep until this message is due. Computed from the FIRST
                // message of the pass rather than from the previous one, so a
                // slow publish does not accumulate into a drift over a long
                // recording.
                if (rate > 0.0)
                {
                    const double offset =
                        static_cast<double>(message.log_time_ns - *first_log_time) / 1e9 / rate;
                    const auto due = wall_start + std::chrono::duration<double>(offset);
                    while (!cli::interrupted() && std::chrono::steady_clock::now() < due)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }

                const std::string key = resolveKey(message.key);

                auto found = publishers.find(key);
                if (found == publishers.end())
                {
                    if (!pub_sub::isValidTopicKey(key))
                    {
                        // A --prefix or --remap that produced something
                        // unpublishable. Counted and reported once at the end
                        // rather than per message.
                        ++skipped_bad_key;
                        return true;
                    }
                    auto publisher = std::make_unique<pub_sub::detail::BytePublisher>(
                        key, message.schema);
                    found = publishers.emplace(key, std::move(publisher)).first;
                }

                if (!found->second->isValid())
                {
                    ++skipped_bad_key;
                    return true;
                }

                // A capnp flat message is always a whole number of 8-byte words.
                // Anything else in the recording is not a message we can
                // republish -- and putting it on the bus would hand every
                // subscriber bytes that decode as a struct full of defaults,
                // which reads exactly like a valid message reporting zeroes.
                if (message.payload.empty() ||
                    message.payload.size() % sizeof(capnp::word) != 0)
                {
                    ++skipped_unaligned;
                    return true;
                }

                kj::Array<capnp::word> words =
                    kj::heapArray<capnp::word>(message.payload.size() / sizeof(capnp::word));
                std::memcpy(words.begin(), message.payload.data(), message.payload.size());

                found->second->put(std::move(words));
                ++published;
                return true;
            });

        if (!ok)
        {
            return cli::kFailure;
        }

    } while (loop && !cli::interrupted());

    cli::out("Published {} message(s) from '{}'.", published, *path);

    if (skipped_unaligned > 0)
    {
        SPDLOG_WARN("{} message(s) were not a whole number of capnp words and were skipped.",
                    skipped_unaligned);
    }
    if (skipped_bad_key > 0)
    {
        SPDLOG_WARN("{} message(s) could not be published -- check --prefix and --remap produce "
                    "valid topic keys.", skipped_bad_key);
    }

    // Let the last messages leave before the session closes underneath them.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    return cli::kOk;
}

}  // namespace bag_tool
