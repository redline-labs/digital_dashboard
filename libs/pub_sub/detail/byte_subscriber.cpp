#include "pub_sub/detail/byte_subscriber.h"

#include <array>
#include <vector>

#include "pub_sub/session_manager.h"
#include "pub_sub/timestamp.h"
#include "pub_sub/topic_key.h"

#include <zenoh.hxx>

#include <capnp/common.h>

#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

namespace pub_sub::detail
{

namespace
{

// SampleMeta over a live zenoh::Sample. Constructed on the stack per sample,
// which costs nothing; the accessors are what would cost, and only run if asked.
class ZenohSampleMeta final : public SampleMeta
{
  public:
    explicit ZenohSampleMeta(const zenoh::Sample& sample) : sample_(sample) {}

    std::string encoding() const override { return sample_.get_encoding().as_string(); }

    std::string_view keyexpr() const override { return sample_.get_keyexpr().as_string_view(); }

    std::optional<std::uint64_t> publishTimeNanos() const override
    {
        const std::optional<zenoh::Timestamp> stamp = sample_.get_timestamp();
        if (!stamp)
        {
            return std::nullopt;
        }
        return ntp64ToUnixNanos(stamp->get_time());
    }

    std::string originZid() const override
    {
        const std::optional<zenoh::Timestamp> stamp = sample_.get_timestamp();
        if (!stamp)
        {
            return {};
        }
        // The id on the timestamp is the session that STAMPED the sample, which
        // is the publisher's -- zenoh stamps at the source when timestamping is
        // enabled there, and a router only fills in for samples that arrive
        // without one.
        //
        // CACHED PER RX THREAD, keyed on the raw 16 bytes. Id::to_string()
        // goes through Rust's formatter and allocates, and a `**` capture
        // called it for every message on the bus -- tens of thousands of
        // times a second to re-format the handful of session ids a bus
        // actually has. thread_local so the cache needs no lock on the one
        // path that must never take one; zenoh delivers a subscriber's
        // callbacks from more than one RX thread.
        const std::array<std::uint8_t, 16>& id = stamp->get_id().bytes();

        thread_local std::vector<std::pair<std::array<std::uint8_t, 16>, std::string>> cache;
        for (const auto& [key, text] : cache)
        {
            if (key == id)
            {
                return text;
            }
        }

        std::string text = stamp->get_id().to_string();
        // Bounded: a runaway set of ids (which would take a bus of hundreds of
        // sessions) degrades to the old per-message formatting, not to growth.
        if (cache.size() < 64)
        {
            cache.emplace_back(id, text);
        }
        return text;
    }

  private:
    const zenoh::Sample& sample_;
};

}  // namespace

struct ByteSubscriber::Impl
{
    // Held so the session outlives the subscription declared on it.
    std::shared_ptr<zenoh::Session> session;
    std::string keyexpr;

    // Declaration order is load-bearing, and this is the whole reason the seam
    // exists as a class rather than a pair of free functions. Members are
    // destroyed in reverse, and zenoh's undeclare joins in-flight callbacks
    // (zenoh-c does `wait_callbacks().wait()`), so `subscriber` MUST come after
    // everything its callback touches -- `handler` above all. With the two the
    // other way round the handler is freed while a callback can still be calling
    // it, which was a use-after-free on every teardown.
    Handler handler;
    std::unique_ptr<zenoh::Subscriber<void>> subscriber;
};

ByteSubscriber::ByteSubscriber(const std::string& keyexpr, Handler on_sample) :
    impl_(std::make_unique<Impl>())
{
    impl_->keyexpr = keyexpr;
    impl_->handler = std::move(on_sample);

    // Weaker than the publisher's rule, and deliberately so: a subscriber may
    // wildcard, and topic discovery subscribes to "**", which is not a valid
    // topic key. What this still catches is a key expression zenoh would reject
    // outright -- without it, KeyExpr's constructor throws below and the catch
    // reports "subscribe failed" without saying that the key itself was the
    // problem.
    if (!isValidSubscribeExpr(impl_->keyexpr))
    {
        SPDLOG_CRITICAL("Refusing to subscribe to '{}': not a usable key expression. Segments may "
                        "contain letters, digits, '_', '-', or be '*' or '**'.",
                        impl_->keyexpr);
        return;
    }

    impl_->session = SessionManager::getOrCreate();
    if (!impl_->session)
    {
        SPDLOG_ERROR("No zenoh session available to subscribe to '{}'", impl_->keyexpr);
        return;
    }

    try
    {
        Impl* const impl = impl_.get();
        impl_->subscriber = std::make_unique<zenoh::Subscriber<void>>(
            impl_->session->declare_subscriber(
                zenoh::KeyExpr(impl_->keyexpr),
                [impl](const zenoh::Sample& sample) {
                    // Nothing may escape into zenoh: this frame unwinds through
                    // the Rust FFI boundary, which aborts. The net has to catch
                    // everything rather than just std::exception -- kj/capnp
                    // throws do derive from it, but that is not a guarantee worth
                    // betting the process on.
                    try
                    {
                        if (impl->handler == nullptr)
                        {
                            return;
                        }
                        const ZenohSampleMeta meta(sample);
                        impl->handler(sample.get_payload().as_vector(), meta);
                    }
                    catch (const std::exception& e)
                    {
                        SPDLOG_ERROR("Error handling zenoh sample on key '{}': {}", impl->keyexpr,
                                     e.what());
                    }
                    catch (...)
                    {
                        SPDLOG_ERROR("Error handling zenoh sample on key '{}'.", impl->keyexpr);
                    }
                },
                zenoh::closures::none));
        SPDLOG_DEBUG("Subscribed to zenoh key '{}'", impl_->keyexpr);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to subscribe to key '{}': {}", impl_->keyexpr, e.what());
    }
}

ByteSubscriber::~ByteSubscriber() = default;

bool ByteSubscriber::isValid() const
{
    return impl_->subscriber != nullptr;
}

std::string_view ByteSubscriber::keyexpr() const
{
    return impl_->keyexpr;
}

void warnPartialWordPayload(std::string_view keyexpr, std::size_t bytes)
{
    SPDLOG_WARN("Key '{}': payload of {} bytes is not a whole number of {}-byte capnp words; "
                "ignoring this sample.",
                keyexpr, bytes, sizeof(capnp::word));
}

}  // namespace pub_sub::detail
