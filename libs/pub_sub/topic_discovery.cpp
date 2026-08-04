#include "pub_sub/topic_discovery.h"

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/session_manager.h"

#include <spdlog/spdlog.h>
#include <zenoh.hxx>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace pub_sub
{

std::vector<TopicObservation> observeTopics(const std::string& keyexpr, int window_ms)
{
    std::vector<TopicObservation> out;

    auto session = SessionManager::getOrCreate();
    if (!session)
    {
        SPDLOG_ERROR("[discovery] failed to obtain a zenoh session");
        return out;
    }

    // The callback runs on a zenoh thread, so everything it touches is behind
    // this mutex and it does no work beyond bookkeeping.
    std::mutex mutex;
    std::map<std::string, TopicObservation> seen;

    try
    {
        auto sub = session->declare_subscriber(
            zenoh::KeyExpr(keyexpr),
            [&](const zenoh::Sample& sample)
            {
                const std::string key(sample.get_keyexpr().as_string_view());
                std::lock_guard<std::mutex> lock(mutex);
                auto& entry = seen[key];
                if (entry.count == 0)
                {
                    entry.key = key;
                    // The schema travels on every sample's encoding, which is
                    // what makes a late-joining observer able to identify a
                    // topic from the first message it happens to catch.
                    entry.schema =
                        std::string(schemaNameFromEncoding(sample.get_encoding().as_string()));
                }
                ++entry.count;
            },
            zenoh::closures::none);

        std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
        (void)sub;  // Torn down here, before the results are read.
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("[discovery] subscribe to '{}' failed: {}", keyexpr, e.what());
        return out;
    }

    std::lock_guard<std::mutex> lock(mutex);
    out.reserve(seen.size());
    for (auto& [key, entry] : seen)
    {
        entry.hz = (window_ms > 0)
                       ? static_cast<double>(entry.count) * 1000.0 / static_cast<double>(window_ms)
                       : 0.0;
        out.push_back(entry);
    }
    return out;
}

std::optional<SampleBytes> readOneSample(const std::string& keyexpr, int timeout_ms)
{
    auto session = SessionManager::getOrCreate();
    if (!session)
    {
        SPDLOG_ERROR("[discovery] failed to obtain a zenoh session");
        return std::nullopt;
    }

    std::mutex mutex;
    std::condition_variable arrived;
    std::optional<SampleBytes> result;

    try
    {
        auto sub = session->declare_subscriber(
            zenoh::KeyExpr(keyexpr),
            [&](const zenoh::Sample& sample)
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (result.has_value())
                {
                    return;
                }
                SampleBytes bytes;
                bytes.key = std::string(sample.get_keyexpr().as_string_view());
                bytes.schema =
                    std::string(schemaNameFromEncoding(sample.get_encoding().as_string()));
                bytes.payload = sample.get_payload().as_vector();
                result = std::move(bytes);
                arrived.notify_all();
            },
            zenoh::closures::none);

        // Wait on the sample rather than sleeping the whole timeout: a topic
        // publishing at 30 Hz should answer in ~33 ms, not in whatever the
        // caller guessed as an upper bound.
        std::unique_lock<std::mutex> lock(mutex);
        arrived.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [&] { return result.has_value(); });
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("[discovery] subscribe to '{}' failed: {}", keyexpr, e.what());
        return std::nullopt;
    }

    return result;
}

}  // namespace pub_sub
