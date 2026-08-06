#include "pub_sub/topic_directory.h"

#include "pub_sub/session_manager.h"
#include "pub_sub/topic_key.h"

#include <zenoh.hxx>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

namespace pub_sub
{

namespace
{

// The liveliness-watching machinery all three directories share.
//
// Everything here was written once for topics and would otherwise have been
// copied twice: the history option and why it is set, the exception net around
// the Rust boundary, and the declaration-order rule that makes teardown safe.
// Copying it would mean three places to get the ordering wrong, and that
// ordering has already been a use-after-free in this tree once.
//
// `on_sample` runs on a zenoh RX thread.
class LivelinessWatcher
{
  public:
    using Apply = std::function<void(const zenoh::Sample&)>;

    LivelinessWatcher(std::string_view keyexpr, std::string_view label, Apply on_sample) :
        label_(label),
        apply_(std::move(on_sample))
    {
        session_ = SessionManager::getOrCreate();
        if (!session_)
        {
            SPDLOG_ERROR("[{}] no zenoh session; advertisements will not be seen", label_);
            return;
        }

        try
        {
            auto options = zenoh::Session::LivelinessSubscriberOptions::create_default();

            // The whole reason there is no initial query and no rescan button:
            // history replays every token declared BEFORE this subscription, so
            // a directory constructed after the publishers still sees all of
            // them. Without it we would need a liveliness_get() as well, and a
            // window in between where a node starting up could be missed by
            // both.
            options.history = true;

            subscriber_ = std::make_unique<zenoh::Subscriber<void>>(
                session_->liveliness_declare_subscriber(
                    zenoh::KeyExpr(std::string(keyexpr)),
                    [this](const zenoh::Sample& sample)
                    {
                        // Nothing may escape into zenoh: the frame above is Rust
                        // and an exception crossing it aborts the process.
                        try
                        {
                            apply_(sample);
                        }
                        catch (const std::exception& e)
                        {
                            SPDLOG_ERROR("[{}] failed to apply an advertisement: {}", label_,
                                         e.what());
                        }
                        catch (...)
                        {
                            SPDLOG_ERROR("[{}] failed to apply an advertisement.", label_);
                        }
                    },
                    zenoh::closures::none, std::move(options)));

            SPDLOG_DEBUG("[{}] watching '{}'", label_, keyexpr);
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[{}] failed to watch '{}': {}", label_, keyexpr, e.what());
        }
    }

    bool isValid() const { return subscriber_ != nullptr; }

  private:
    std::string label_;
    Apply apply_;
    std::shared_ptr<zenoh::Session> session_;

    // Declared last so it is destroyed FIRST. Undeclaring joins in-flight
    // callbacks, so once it is gone nothing can still be running `apply_` or
    // touching whatever that closure captured. The same ordering rule as
    // ByteSubscriber, and it has been a use-after-free in this tree before --
    // do not move it.
    std::unique_ptr<zenoh::Subscriber<void>> subscriber_;
};

// PUT means the token was declared; DELETE means it was undeclared, its process
// exited, or we lost connectivity to it. All three mean the same thing to us: we
// cannot currently reach whoever declared it.
bool isPresent(const zenoh::Sample& sample)
{
    return sample.get_kind() == zenoh::SampleKind::Z_SAMPLE_KIND_PUT;
}

void recordPresence(DirectoryPresence& entry, bool present)
{
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

}  // namespace

struct TopicDirectory::Impl
{
    // Guards `entries` and `revision`. Held only for the map update or the
    // snapshot copy -- never across anything that could block, since the write
    // side is a zenoh RX thread.
    mutable std::mutex mutex;
    std::map<std::string, DirectoryEntry> entries;
    std::atomic<std::uint64_t> revision{0};

    void apply(const zenoh::Sample& sample)
    {
        const std::string advertised(sample.get_keyexpr().as_string_view());

        std::string topic;
        std::string schema;
        std::string zid;
        if (!parseAdvertiseKey(advertised, topic, schema, zid))
        {
            // Not an advertisement at all, or one whose topic segment does not
            // demangle into something bindable. Skipping beats guessing, and it
            // is logged rather than silently dropped.
            SPDLOG_DEBUG("[directory] ignoring unrecognised advertisement '{}'", advertised);
            return;
        }

        const bool present = isPresent(sample);

        {
            const std::lock_guard<std::mutex> guard(mutex);

            DirectoryEntry& entry = entries[topic];
            entry.key = topic;
            entry.schema = schema;

            // Only overwritten when the advertiser told us. An older publisher
            // carries no zid, and letting that blank out an owner we already
            // learned -- from a second publisher of the same topic, or from an
            // earlier appearance of this one -- would lose information rather
            // than correct it.
            if (!zid.empty())
            {
                entry.owner_zid = zid;
            }

            recordPresence(entry, present);
        }

        revision.fetch_add(1, std::memory_order_relaxed);
    }

    // Declared last so it is destroyed FIRST -- see LivelinessWatcher, which
    // holds the same rule for the same reason.
    std::unique_ptr<LivelinessWatcher> watcher;
};

TopicDirectory::TopicDirectory() : impl_(std::make_unique<Impl>())
{
    Impl* const impl = impl_.get();
    impl_->watcher = std::make_unique<LivelinessWatcher>(
        kAdvertiseAll, "directory", [impl](const zenoh::Sample& sample) { impl->apply(sample); });
}

TopicDirectory::~TopicDirectory() = default;

bool TopicDirectory::isValid() const
{
    return impl_->watcher && impl_->watcher->isValid();
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

// ------------------------------------------------------------- NodeDirectory

struct NodeDirectory::Impl
{
    mutable std::mutex mutex;

    // Keyed on zid, not name: two instances of the same node are two entries,
    // and keying on the name would have the second silently replace the first.
    // Two dashboards running at once is a mistake worth being able to SEE.
    std::map<std::string, NodeEntry> entries;
    std::atomic<std::uint64_t> revision{0};

    void apply(const zenoh::Sample& sample)
    {
        const std::string advertised(sample.get_keyexpr().as_string_view());

        std::string zid;
        std::string name;
        if (!parseNodeKey(advertised, zid, name))
        {
            SPDLOG_DEBUG("[nodes] ignoring unrecognised node advertisement '{}'", advertised);
            return;
        }

        const bool present = isPresent(sample);

        {
            const std::lock_guard<std::mutex> guard(mutex);

            NodeEntry& entry = entries[zid];
            entry.zid = zid;
            entry.name = name;
            recordPresence(entry, present);
        }

        revision.fetch_add(1, std::memory_order_relaxed);
    }

    std::unique_ptr<LivelinessWatcher> watcher;
};

NodeDirectory::NodeDirectory() : impl_(std::make_unique<Impl>())
{
    Impl* const impl = impl_.get();
    impl_->watcher = std::make_unique<LivelinessWatcher>(
        kNodeAll, "nodes", [impl](const zenoh::Sample& sample) { impl->apply(sample); });
}

NodeDirectory::~NodeDirectory() = default;

bool NodeDirectory::isValid() const
{
    return impl_->watcher && impl_->watcher->isValid();
}

std::vector<NodeEntry> NodeDirectory::snapshot() const
{
    std::vector<NodeEntry> out;

    {
        const std::lock_guard<std::mutex> guard(impl_->mutex);
        out.reserve(impl_->entries.size());
        for (const auto& [zid, entry] : impl_->entries)
        {
            out.push_back(entry);
        }
    }

    // By name first, so two instances of one node land next to each other.
    std::sort(out.begin(), out.end(),
              [](const NodeEntry& lhs, const NodeEntry& rhs)
              {
                  if (lhs.name != rhs.name)
                  {
                      return lhs.name < rhs.name;
                  }
                  return lhs.zid < rhs.zid;
              });
    return out;
}

std::uint64_t NodeDirectory::revision() const
{
    return impl_->revision.load(std::memory_order_relaxed);
}

std::string NodeDirectory::nameFor(std::string_view zid) const
{
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto it = impl_->entries.find(std::string(zid));
    return it != impl_->entries.end() ? it->second.name : std::string();
}

// ---------------------------------------------------------- ServiceDirectory

struct ServiceDirectory::Impl
{
    mutable std::mutex mutex;

    // Keyed on the service's key expression, which is what a caller addresses.
    std::map<std::string, ServiceEntry> entries;
    std::atomic<std::uint64_t> revision{0};

    void apply(const zenoh::Sample& sample)
    {
        const std::string advertised(sample.get_keyexpr().as_string_view());

        std::string key;
        std::string request_schema;
        std::string response_schema;
        std::string zid;
        if (!parseServiceKey(advertised, key, request_schema, response_schema, zid))
        {
            SPDLOG_DEBUG("[services] ignoring unrecognised service advertisement '{}'", advertised);
            return;
        }

        const bool present = isPresent(sample);

        {
            const std::lock_guard<std::mutex> guard(mutex);

            ServiceEntry& entry = entries[key];
            entry.key = key;
            entry.request_schema = request_schema;
            entry.response_schema = response_schema;
            entry.owner_zid = zid;
            recordPresence(entry, present);
        }

        revision.fetch_add(1, std::memory_order_relaxed);
    }

    std::unique_ptr<LivelinessWatcher> watcher;
};

ServiceDirectory::ServiceDirectory() : impl_(std::make_unique<Impl>())
{
    Impl* const impl = impl_.get();
    impl_->watcher = std::make_unique<LivelinessWatcher>(
        kServiceAll, "services", [impl](const zenoh::Sample& sample) { impl->apply(sample); });
}

ServiceDirectory::~ServiceDirectory() = default;

bool ServiceDirectory::isValid() const
{
    return impl_->watcher && impl_->watcher->isValid();
}

std::vector<ServiceEntry> ServiceDirectory::snapshot() const
{
    std::vector<ServiceEntry> out;

    {
        const std::lock_guard<std::mutex> guard(impl_->mutex);
        out.reserve(impl_->entries.size());
        for (const auto& [key, entry] : impl_->entries)
        {
            out.push_back(entry);
        }
    }

    std::sort(out.begin(), out.end(),
              [](const ServiceEntry& lhs, const ServiceEntry& rhs) { return lhs.key < rhs.key; });
    return out;
}

std::uint64_t ServiceDirectory::revision() const
{
    return impl_->revision.load(std::memory_order_relaxed);
}

}  // namespace pub_sub
