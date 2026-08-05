#include "pub_sub/detail/byte_subscriber.h"

#include "pub_sub/session_manager.h"

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
// which costs nothing; encoding() is what would cost, and only runs if asked.
class ZenohSampleMeta final : public SampleMeta
{
  public:
    explicit ZenohSampleMeta(const zenoh::Sample& sample) : sample_(sample) {}

    std::string encoding() const override { return sample_.get_encoding().as_string(); }

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

    if (impl_->keyexpr.empty())
    {
        SPDLOG_ERROR("Refusing to subscribe to an empty key expression");
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
