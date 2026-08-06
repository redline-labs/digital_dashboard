#include "pub_sub/topic_directory.h"

#include "pub_sub/session_manager.h"
#include "pub_sub/topic_key.h"

#include <zenoh.hxx>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace pub_sub
{

struct TopicDirectory::Impl
{
    // Guards `entries` and `revision`. Held only for the map update or the
    // snapshot copy -- never across anything that could block, since the write
    // side is a zenoh RX thread.
    mutable std::mutex mutex;
    std::map<std::string, DirectoryEntry> entries;
    std::atomic<std::uint64_t> revision{0};

    std::shared_ptr<zenoh::Session> session;

    void apply(const zenoh::Sample& sample)
    {
        const std::string advertised(sample.get_keyexpr().as_string_view());

        std::string topic;
        std::string schema;
        if (!parseAdvertiseKey(advertised, topic, schema))
        {
            // A form this build does not know -- a newer node advertising a
            // longer key, say. Skipping beats guessing which segment means
            // what, and it is logged once per key rather than silently.
            SPDLOG_DEBUG("[directory] ignoring unrecognised advertisement '{}'", advertised);
            return;
        }

        // PUT means the token was declared; DELETE means it was undeclared, its
        // process exited, or we lost connectivity to it. All three mean the same
        // thing to us: we cannot currently reach whoever publishes this.
        const bool present = sample.get_kind() == zenoh::SampleKind::Z_SAMPLE_KIND_PUT;

        {
            const std::lock_guard<std::mutex> guard(mutex);

            DirectoryEntry& entry = entries[topic];
            entry.key = topic;
            entry.schema = schema;
            entry.reachable = present;
            if (present)
            {
                ++entry.appearances;
            }
            else
            {
                ++entry.disappearances;
            }
        }

        revision.fetch_add(1, std::memory_order_relaxed);
    }

    // Declared last so it is destroyed FIRST. Undeclaring joins in-flight
    // callbacks, so once it is gone nothing can still be touching the map or
    // the mutex above. The same ordering rule as ByteSubscriber, and it has
    // been a use-after-free in this tree before -- do not move it.
    std::unique_ptr<zenoh::Subscriber<void>> subscriber;
};

TopicDirectory::TopicDirectory() : impl_(std::make_unique<Impl>())
{
    impl_->session = SessionManager::getOrCreate();
    if (!impl_->session)
    {
        SPDLOG_ERROR("[directory] no zenoh session; topic advertisements will not be seen");
        return;
    }

    try
    {
        Impl* const impl = impl_.get();

        auto options = zenoh::Session::LivelinessSubscriberOptions::create_default();

        // The whole reason there is no initial query and no rescan button:
        // history replays every token declared BEFORE this subscription, so a
        // directory constructed after the publishers still sees all of them.
        // Without it we would need a liveliness_get() as well, and a window in
        // between where a node starting up could be missed by both.
        options.history = true;

        impl_->subscriber = std::make_unique<zenoh::Subscriber<void>>(
            impl_->session->liveliness_declare_subscriber(
                zenoh::KeyExpr(kAdvertiseAll),
                [impl](const zenoh::Sample& sample) {
                    // Nothing may escape into zenoh: the frame above is Rust and
                    // an exception crossing it aborts the process.
                    try
                    {
                        impl->apply(sample);
                    }
                    catch (const std::exception& e)
                    {
                        SPDLOG_ERROR("[directory] failed to apply an advertisement: {}", e.what());
                    }
                    catch (...)
                    {
                        SPDLOG_ERROR("[directory] failed to apply an advertisement.");
                    }
                },
                zenoh::closures::none, std::move(options)));

        SPDLOG_DEBUG("[directory] watching '{}'", kAdvertiseAll);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("[directory] failed to watch '{}': {}", kAdvertiseAll, e.what());
    }
}

TopicDirectory::~TopicDirectory() = default;

bool TopicDirectory::isValid() const
{
    return impl_->subscriber != nullptr;
}

std::vector<DirectoryEntry> TopicDirectory::snapshot() const
{
    std::vector<DirectoryEntry> out;

    {
        const std::lock_guard<std::mutex> guard(impl_->mutex);
        out.reserve(impl_->entries.size());
        for (const auto& [key, entry] : impl_->entries)
        {
            out.push_back(entry);
        }
    }

    // std::map already iterates in key order, so this is only insurance against
    // the container changing later.
    std::sort(out.begin(), out.end(),
              [](const DirectoryEntry& lhs, const DirectoryEntry& rhs) { return lhs.key < rhs.key; });
    return out;
}

std::uint64_t TopicDirectory::revision() const
{
    return impl_->revision.load(std::memory_order_relaxed);
}

}  // namespace pub_sub
