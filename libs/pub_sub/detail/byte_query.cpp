#include "pub_sub/detail/byte_query.h"
#include "layout_attachment.h"

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/topic_key.h"

#include <zenoh.hxx>

#include <spdlog/spdlog.h>

#include <utility>

namespace pub_sub::detail
{

namespace
{

ByteMessage messageFrom(const zenoh::Bytes& payload, const zenoh::Encoding& encoding,
                        const std::optional<std::reference_wrapper<const zenoh::Bytes>>& attachment)
{
    ByteMessage message;
    message.payload = payload.as_vector();
    message.schema_name = std::string(schemaNameFromEncoding(encoding.as_string()));
    if (attachment)
    {
        message.layout = layoutFromAttachment(attachment->get());
    }
    return message;
}

zenoh::Encoding capnpEncoding(std::string_view schema)
{
    zenoh::Encoding encoding(kCapnpEncodingMime);
    encoding.set_schema(std::string(schema));
    return encoding;
}

}  // namespace

struct ByteQueryable::Impl
{
    std::string keyexpr;
    std::string response_schema;
    std::uint64_t response_layout{0};
    Handler handler;
    std::shared_ptr<zenoh::Session> session;
    std::optional<zenoh::LivelinessToken> advertisement;
    // Last, so it is undeclared first: that joins the callback, which uses
    // everything above.
    std::optional<zenoh::Queryable<void>> queryable;

    void reply(const zenoh::Query& query, Answer answer) const
    {
        if (!answer.error.empty())
        {
            zenoh::Query::ReplyErrOptions options = zenoh::Query::ReplyErrOptions::create_default();
            options.encoding.emplace(zenoh::Encoding::Predefined::zenoh_string());
            query.reply_err(zenoh::Bytes(std::move(answer.error)), std::move(options));
            return;
        }
        zenoh::Query::ReplyOptions options = zenoh::Query::ReplyOptions::create_default();
        options.encoding.emplace(capnpEncoding(response_schema));
        if (auto stamp = layoutAttachment(response_layout))
        {
            options.attachment.emplace(std::move(*stamp));
        }
        query.reply(keyexpr, zenoh::Bytes(std::move(answer.payload)), std::move(options));
    }
};

ByteQueryable::ByteQueryable(std::string keyexpr, std::string_view request_schema,
                             std::string_view response_schema, std::uint64_t response_layout,
                             Handler handler) :
    impl_(std::make_unique<Impl>())
{
    impl_->keyexpr = std::move(keyexpr);
    impl_->response_schema = std::string(response_schema);
    impl_->response_layout = response_layout;
    impl_->handler = std::move(handler);
    impl_->session = SessionManager::getOrCreate();
    if (!impl_->session)
    {
        SPDLOG_ERROR("No zenoh session available to serve '{}'", impl_->keyexpr);
        return;
    }

    Impl* const impl = impl_.get();
    try
    {
        impl_->queryable = impl_->session->declare_queryable(
            zenoh::KeyExpr(impl_->keyexpr),
            [impl](const zenoh::Query& query) {
                // Nothing may escape into zenoh's Rust frame: a throw there
                // aborts the process.
                try
                {
                    const auto payload = query.get_payload();
                    const auto encoding = query.get_encoding();
                    ByteMessage request;
                    if (payload && encoding)
                    {
                        request = messageFrom(payload->get(), encoding->get(), query.get_attachment());
                    }
                    else if (payload)
                    {
                        request = messageFrom(payload->get(), zenoh::Encoding(), query.get_attachment());
                    }
                    impl->reply(query, impl->handler(request));
                }
                catch (const std::exception& e)
                {
                    SPDLOG_ERROR("Service '{}' failed a request: {}", impl->keyexpr, e.what());
                    try
                    {
                        impl->reply(query, Answer{{}, e.what()});
                    }
                    catch (...)
                    {
                    }
                }
                catch (...)
                {
                    SPDLOG_ERROR("Service '{}' failed a request with a non-standard exception",
                                 impl->keyexpr);
                }
            },
            [] {}, zenoh::Session::QueryableOptions::create_default());
        SPDLOG_DEBUG("Service active on '{}' for schemas '{}'->'{}'", impl_->keyexpr,
                     request_schema, response_schema);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to serve '{}': {}", impl_->keyexpr, e.what());
        return;
    }

    try
    {
        const std::string advertised = serviceKey(impl_->keyexpr, request_schema, response_schema,
                                                  impl_->session->get_zid().to_string());
        impl_->advertisement.emplace(
            impl_->session->liveliness_declare_token(zenoh::KeyExpr(advertised)));
        SPDLOG_DEBUG("Advertised service '{}' as '{}'", impl_->keyexpr, advertised);
    }
    catch (const std::exception& e)
    {
        SPDLOG_WARN("Service on '{}' is running but could not be advertised: {}", impl_->keyexpr,
                    e.what());
    }
}

ByteQueryable::~ByteQueryable()
{
    if (impl_ && impl_->queryable.has_value())
    {
        std::move(*impl_->queryable).undeclare();
        impl_->queryable.reset();
    }
}

bool ByteQueryable::isValid() const
{
    return impl_->queryable.has_value();
}

bool queryBytes(const std::string& keyexpr, std::string_view request_schema,
                std::uint64_t request_layout, std::vector<std::uint8_t> request,
                std::uint64_t timeout_ms, std::function<void(const ByteMessage&)> on_reply,
                std::function<void()> on_done)
{
    const std::shared_ptr<zenoh::Session> session = SessionManager::getOrCreate();
    if (!session)
    {
        SPDLOG_ERROR("No zenoh session available to query '{}'", keyexpr);
        return false;
    }

    try
    {
        zenoh::Session::GetOptions options = zenoh::Session::GetOptions::create_default();
        options.timeout_ms = timeout_ms;
        options.payload.emplace(std::move(request));
        options.encoding.emplace(capnpEncoding(request_schema));
        if (auto stamp = layoutAttachment(request_layout))
        {
            options.attachment.emplace(std::move(*stamp));
        }

        session->get(
            zenoh::KeyExpr(keyexpr), "",
            [handle = std::move(on_reply), keyexpr](const zenoh::Reply& reply) {
                try
                {
                    if (!reply.is_ok())
                    {
                        return;
                    }
                    const zenoh::Sample& sample = reply.get_ok();
                    handle(messageFrom(sample.get_payload(), sample.get_encoding(),
                                       sample.get_attachment()));
                }
                catch (const std::exception& e)
                {
                    SPDLOG_ERROR("A reply from '{}' could not be handled: {}", keyexpr, e.what());
                }
                catch (...)
                {
                    SPDLOG_ERROR("A reply from '{}' could not be handled", keyexpr);
                }
            },
            [finish = std::move(on_done)] {
                try
                {
                    finish();
                }
                catch (...)
                {
                    SPDLOG_ERROR("A query's completion handler threw");
                }
            },
            std::move(options));
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Request to '{}' failed: {}", keyexpr, e.what());
        return false;
    }
    return true;
}

}  // namespace pub_sub::detail
