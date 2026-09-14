#include "pub_sub/dynamic_service_call.h"

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/session_manager.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>

#include <zenoh.hxx>

#include <spdlog/spdlog.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>

namespace pub_sub
{

namespace
{

using Clock = std::chrono::steady_clock;

// One call's worth of state, owned by the two zenoh closures. Whichever of them
// runs last releases it, so nothing here outlives or predates the query.
struct PendingCall
{
    std::function<void(ServiceCallResult)> done;
    std::string response_schema;
    CapnpJsonOptions decode_options;
    Clock::time_point started;

    std::mutex mutex;
    std::vector<ServiceReply> replies;
    std::atomic<bool> delivered{false};

    void add(ServiceReply reply)
    {
        const std::lock_guard<std::mutex> guard(mutex);
        replies.push_back(std::move(reply));
    }

    // exchange, not load-then-store: the reply callback and the drop handler
    // run on zenoh threads, and the promise to the caller is one call.
    void finish()
    {
        if (delivered.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        ServiceCallResult result;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            result.replies = std::move(replies);
        }
        result.status = result.replies.empty() ? ServiceCallResult::Status::NoReply
                                               : ServiceCallResult::Status::Replied;
        result.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        done(std::move(result));
    }
};

ServiceReply errorReply(std::string text)
{
    ServiceReply out;
    out.is_error = true;
    out.error_text = std::move(text);
    return out;
}

ServiceReply decodeReply(const zenoh::Reply& reply, const PendingCall& call)
{
    ServiceReply out;

    if (!reply.is_ok())
    {
        // A service that failed the request -- ZenohService answers a throwing
        // handler this way. The payload is text for a person.
        out.is_error = true;
        out.error_text = reply.get_err().get_payload().as_string();
        if (out.error_text.empty())
        {
            out.error_text = "the service replied with an error and no reason";
        }
        return out;
    }

    const zenoh::Sample& sample = reply.get_ok();
    const std::string stamped(schemaNameFromEncoding(sample.get_encoding().as_string()));
    out.schema = stamped.empty() ? call.response_schema : stamped;

    const auto schema = get_schema(out.schema);
    if (!schema)
    {
        out.is_error = true;
        out.error_text = out.schema.empty()
                             ? "the reply names no schema, and none was advertised"
                             : "reply schema '" + out.schema + "' is not in this build's registry";
        return out;
    }

    try
    {
        out.value = capnpToJson(sample.get_payload().as_vector(), *schema, call.decode_options);
    }
    catch (const kj::Exception& e)
    {
        out.is_error = true;
        out.error_text = std::string("the reply could not be decoded as ") + out.schema + ": " +
                         e.getDescription().cStr();
    }
    return out;
}

ServiceCallResult notSent(ServiceCallResult::Status status, std::vector<std::string> errors)
{
    ServiceCallResult result;
    result.status = status;
    result.errors = std::move(errors);
    return result;
}

}  // namespace

const char* to_string(ServiceCallResult::Status status)
{
    switch (status)
    {
        case ServiceCallResult::Status::Replied:
            return "replied";
        case ServiceCallResult::Status::NoReply:
            return "no reply";
        case ServiceCallResult::Status::RequestRejected:
            return "request rejected";
        case ServiceCallResult::Status::Failed:
            return "failed";
    }
    return "unknown";
}

bool callService(ServiceCallRequest request, std::function<void(ServiceCallResult)> done)
{
    const auto schema = get_schema(request.request_schema);
    if (!schema)
    {
        done(notSent(ServiceCallResult::Status::RequestRejected,
                     {"request schema '" + request.request_schema +
                      "' is not in this build's registry"}));
        return false;
    }

    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(schema->asStruct());

    std::vector<std::string> errors;
    if (!jsonToCapnp(request.fields, root, errors))
    {
        done(notSent(ServiceCallResult::Status::RequestRejected, std::move(errors)));
        return false;
    }

    auto session = SessionManager::getOrCreate();
    if (!session)
    {
        done(notSent(ServiceCallResult::Status::Failed, {"no zenoh session available"}));
        return false;
    }

    const auto words = capnp::messageToFlatArray(message);
    const auto bytes = words.asBytes();

    auto call = std::make_shared<PendingCall>();
    call->done = std::move(done);
    call->response_schema = std::move(request.response_schema);
    call->decode_options = request.decode_options;

    try
    {
        zenoh::Session::GetOptions options = zenoh::Session::GetOptions::create_default();
        options.timeout_ms = static_cast<std::uint64_t>(request.timeout.count());

        // EVERY queryable, and every reply kept. zenoh's defaults are the
        // opposite on both counts: the target is "best matching", and replies
        // are consolidated by key -- two services answering on the same key
        // expression reach the caller as ONE reply, the other silently
        // discarded. That hides exactly the misconfiguration a person calling
        // a service by hand needs to see.
        options.target = zenoh::QueryTarget::Z_QUERY_TARGET_ALL;
        options.consolidation =
            zenoh::QueryConsolidation(zenoh::ConsolidationMode::Z_CONSOLIDATION_MODE_NONE);
        options.payload.emplace(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
        options.encoding.emplace(kCapnpEncodingMime);
        options.encoding->set_schema(request.request_schema);

        call->started = Clock::now();
        session->get(
            zenoh::KeyExpr(request.key), "",
            [call](const zenoh::Reply& reply)
            {
                // Nothing may escape into zenoh's Rust frame.
                try
                {
                    call->add(decodeReply(reply, *call));
                }
                catch (const std::exception& e)
                {
                    call->add(errorReply(std::string("reply unreadable: ") + e.what()));
                }
                catch (...)
                {
                    call->add(errorReply("reply unreadable"));
                }
            },
            [call]()
            {
                // The drop handler: the query is over, by reply-and-done or by
                // timeout. The one place a result is delivered from once sent.
                try
                {
                    call->finish();
                }
                catch (...)
                {
                    SPDLOG_ERROR("A service call's completion handler threw");
                }
            },
            std::move(options));
    }
    catch (const std::exception& e)
    {
        // Not sent, so the drop handler will not run: deliver here instead.
        if (!call->delivered.exchange(true, std::memory_order_acq_rel))
        {
            call->done(notSent(ServiceCallResult::Status::Failed,
                               {std::string("the query could not be sent: ") + e.what()}));
        }
        return false;
    }

    return true;
}

ServiceCallResult callServiceBlocking(ServiceCallRequest request)
{
    auto promise = std::make_shared<std::promise<ServiceCallResult>>();
    auto future = promise->get_future();
    const auto timeout = request.timeout;

    callService(std::move(request),
                [promise](ServiceCallResult result) { promise->set_value(std::move(result)); });

    // zenoh always ends a query at its timeout, so this only guards against a
    // drop handler that never comes -- a hang in a CLI is worse than an answer.
    if (future.wait_for(timeout + std::chrono::seconds(5)) != std::future_status::ready)
    {
        return notSent(ServiceCallResult::Status::Failed,
                       {"zenoh never reported the query finished"});
    }
    return future.get();
}

}  // namespace pub_sub
