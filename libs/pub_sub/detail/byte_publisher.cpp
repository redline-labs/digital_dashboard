#include "pub_sub/detail/byte_publisher.h"

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/session_manager.h"

#include <zenoh.hxx>

#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace pub_sub::detail
{

struct BytePublisher::Impl
{
    // Held so the session outlives the publisher declared on it.
    std::shared_ptr<zenoh::Session> session;
    std::unique_ptr<zenoh::Publisher> publisher;

    // Our own copy of both strings. The key is returned by keyexpr() as a view,
    // and zenoh's own get_keyexpr() would hand back a view into memory this class
    // no longer promises to keep. The schema name is read on every put().
    std::string keyexpr;
    std::string schema_name;
};

BytePublisher::BytePublisher(std::string_view keyexpr, std::string_view schema_name) :
    impl_(std::make_unique<Impl>())
{
    impl_->keyexpr = std::string(keyexpr);
    impl_->schema_name = std::string(schema_name);

    impl_->session = SessionManager::getOrCreate();
    if (!impl_->session)
    {
        SPDLOG_ERROR("No zenoh session available to publish on '{}'", impl_->keyexpr);
        return;
    }

    try
    {
        impl_->publisher = std::make_unique<zenoh::Publisher>(
            impl_->session->declare_publisher(impl_->keyexpr));
        SPDLOG_DEBUG("Publisher active on '{}' for schema '{}'", impl_->keyexpr,
                     impl_->schema_name);
    }
    catch (const std::exception& e)
    {
        // The old code called declare_publisher() straight out of a member
        // initializer, through a session pointer it had not checked -- so a host
        // with no session segfaulted on construction instead of reporting it.
        SPDLOG_ERROR("Failed to declare publisher on '{}': {}", impl_->keyexpr, e.what());
    }
}

BytePublisher::~BytePublisher() = default;

bool BytePublisher::isValid() const
{
    return impl_->publisher != nullptr;
}

std::string_view BytePublisher::keyexpr() const
{
    return impl_->keyexpr;
}

void BytePublisher::put(kj::Array<capnp::word> payload)
{
    if (impl_->publisher == nullptr)
    {
        return;
    }

    // Zero-copy handover. The owning kj::Array moves into a shared_ptr that the
    // deleter lambda captures; zenoh stores that callable and invokes it when it
    // is done with the payload, which destroys the lambda, the shared_ptr, and
    // finally the buffer -- at the right time rather than at the end of this
    // function. The uint8_t* the deleter is handed is ignored: it only ever
    // aliases memory the captured array owns.
    auto owner = std::make_shared<kj::Array<capnp::word>>(kj::mv(payload));
    const auto bytes = owner->asBytes();
    auto* const ptr = reinterpret_cast<std::uint8_t*>(const_cast<kj::byte*>(bytes.begin()));
    const std::size_t len = bytes.size();

    auto deleter = [owner = std::move(owner)](std::uint8_t*) noexcept {
        // Nothing to do: `owner` is destroyed with this lambda, freeing the
        // buffer.
    };

    auto opts = zenoh::Publisher::PutOptions::create_default();
    opts.encoding.emplace(kCapnpEncodingMime);
    // set_schema() takes a string_view, so this makes no temporary.
    opts.encoding->set_schema(impl_->schema_name);

    try
    {
        impl_->publisher->put(zenoh::Bytes(ptr, len, std::move(deleter)), std::move(opts));
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to publish on '{}': {}", impl_->keyexpr, e.what());
    }
}

bool BytePublisher::hasSubscribers() const
{
    if (impl_->publisher == nullptr)
    {
        return false;
    }
    return impl_->publisher->get_matching_status().matching;
}

void BytePublisher::onSubscriberPresenceChanged(std::function<void(bool present)> handler)
{
    if (impl_->publisher == nullptr)
    {
        return;
    }

    impl_->publisher->declare_background_matching_listener(
        [handler = std::move(handler), key = impl_->keyexpr](const zenoh::MatchingStatus& status) {
            // This frame sits on zenoh's Rust FFI boundary; letting anything
            // unwind past it aborts the process.
            try
            {
                handler(status.matching);
            }
            catch (...)
            {
                SPDLOG_ERROR("Subscriber-presence handler threw for key '{}'.", key);
            }
        },
        []() {});
}

}  // namespace pub_sub::detail
