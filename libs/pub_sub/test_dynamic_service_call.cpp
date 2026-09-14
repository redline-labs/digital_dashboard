// SPDX-License-Identifier: GPL-3.0-or-later
//
// callService: a service call where the schemas are known only by name.
//
// The cases that matter are the ones a tool calling arbitrary services will hit
// and a typed client never does:
//
//   * a handler that THROWS. ZenohService used to let that exception cross into
//     zenoh's Rust frame, which aborts the process -- one bad request took down
//     the node and every other service on it, and the caller saw only a
//     timeout. It must now come back as an error reply carrying the reason, and
//     the service must still be there for the next call.
//     Mutation-check: remove the try/catch in ZenohService's on_query and this
//     test aborts instead of passing.
//
//   * two responders on one key. Both replies must be collected; a first-wins
//     client hides exactly the misconfiguration a person calling by hand needs
//     to see.
//
//   * a request that does not fit its schema is refused BEFORE anything is
//     sent, so the service never sees half a request.
//
// Services and caller share this process's session, which is how zenoh routes a
// local query to a local queryable. `net`.

#include "pub_sub/dynamic_service_call.h"
#include "pub_sub/zenoh_service.h"

#include "bd992.capnp.h"
#include "can_bridge.capnp.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using Status = pub_sub::ServiceCallResult::Status;
using Bd992Service = pub_sub::ZenohService<Bd992SendCommandRequest, Bd992SendCommandResponse>;
using BitrateService =
    pub_sub::ZenohService<CanBridgeSetBitrateRequest, CanBridgeSetBitrateResponse>;

pub_sub::ServiceCallRequest bd992Request(const std::string& key, pub_sub::json fields)
{
    pub_sub::ServiceCallRequest request;
    request.key = key;
    request.request_schema = "Bd992SendCommandRequest";
    request.response_schema = "Bd992SendCommandResponse";
    request.fields = std::move(fields);
    request.timeout = std::chrono::milliseconds(2000);
    request.decode_options.data_hex_limit = 64;
    return request;
}

// Data both ways: the request's hex becomes bytes the handler sees, and the
// bytes it sends back become hex again.
void test_data_round_trips_through_a_real_service()
{
    Bd992Service service("test/dynamic_call/echo",
                         [](const Bd992SendCommandRequest::Reader& request,
                            Bd992SendCommandResponse::Builder& response)
                         {
                             response.setOk(true);
                             response.setReplyType(request.getPacketType());
                             response.setReplyData(request.getData());
                         });

    const auto result = pub_sub::callServiceBlocking(
        bd992Request("test/dynamic_call/echo", {{"packetType", 0x56}, {"data", "01 02 ff"}}));

    check(result.status == Status::Replied, "a served key replies");
    check(result.replies.size() == 1, "one service, one reply");
    if (result.replies.size() == 1)
    {
        const auto& reply = result.replies[0];
        check(!reply.is_error, "the reply is not an error: " + reply.error_text);
        check(reply.schema == "Bd992SendCommandResponse", "the reply names its schema");
        check(reply.value.value("ok", false), "the handler's answer came back");
        check(reply.value.value("replyType", 0) == 0x56, "an integer made the round trip");
        check(reply.value.value("replyData", std::string()) == "0102ff",
              "Data went in as hex, reached the handler as bytes, and came back as hex");
    }
}

void test_a_bad_request_is_never_sent()
{
    std::atomic<int> calls{0};
    Bd992Service service("test/dynamic_call/guarded",
                         [&calls](const Bd992SendCommandRequest::Reader&,
                                  Bd992SendCommandResponse::Builder&) { ++calls; });

    const auto unknown_field = pub_sub::callServiceBlocking(
        bd992Request("test/dynamic_call/guarded", {{"packetTyp", 1}}));
    check(unknown_field.status == Status::RequestRejected, "an unknown field rejects the request");
    check(!unknown_field.errors.empty() &&
              unknown_field.errors[0].find("no such field") != std::string::npos,
          "and says which field");

    auto bad_schema = bd992Request("test/dynamic_call/guarded", pub_sub::json::object());
    bad_schema.request_schema = "NoSuchSchema";
    check(pub_sub::callServiceBlocking(bad_schema).status == Status::RequestRejected,
          "an unknown request schema rejects the request");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(calls.load() == 0, "neither rejected request reached the service");
}

void test_nobody_home_is_no_reply()
{
    auto request = bd992Request("test/dynamic_call/nobody_home", pub_sub::json::object());
    request.timeout = std::chrono::milliseconds(300);
    const auto result = pub_sub::callServiceBlocking(request);
    check(result.status == Status::NoReply, "an unserved key is NoReply");
    check(result.replies.empty(), "with no replies");
}

void test_a_throwing_handler_answers_with_its_reason()
{
    std::atomic<int> calls{0};
    BitrateService service("test/dynamic_call/throws",
                           [&calls](const CanBridgeSetBitrateRequest::Reader&,
                                    CanBridgeSetBitrateResponse::Builder&)
                           {
                               ++calls;
                               throw std::runtime_error("controller refused 123 bit/s");
                           });

    pub_sub::ServiceCallRequest request;
    request.key = "test/dynamic_call/throws";
    request.request_schema = "CanBridgeSetBitrateRequest";
    request.fields = {{"channel", "can0"}, {"nominalBps", 123}};

    for (int attempt = 1; attempt <= 2; ++attempt)
    {
        const auto result = pub_sub::callServiceBlocking(request);
        const std::string which = attempt == 1 ? "the first call" : "a second call";
        check(result.status == Status::Replied, which + " to a throwing handler still replies");
        check(result.replies.size() == 1 && result.replies[0].is_error,
              which + " comes back as an error reply");
        check(result.replies.size() == 1 &&
                  result.replies[0].error_text.find("controller refused 123 bit/s") !=
                      std::string::npos,
              which + " carries the exception's message");
    }
    check(calls.load() == 2, "the process survived the first throw to handle the second call");
}

void test_every_responder_is_collected()
{
    const auto handler = [](std::uint8_t type)
    {
        return [type](const Bd992SendCommandRequest::Reader&,
                      Bd992SendCommandResponse::Builder& response)
        {
            response.setOk(true);
            response.setReplyType(type);
        };
    };
    Bd992Service first("test/dynamic_call/twice", handler(1));
    Bd992Service second("test/dynamic_call/twice", handler(2));

    const auto result = pub_sub::callServiceBlocking(
        bd992Request("test/dynamic_call/twice", pub_sub::json::object()));
    check(result.status == Status::Replied, "a doubly-served key replies");
    check(result.replies.size() == 2, "and both replies are collected, not just the first");

    int seen = 0;
    for (const auto& reply : result.replies)
    {
        seen |= reply.value.value("replyType", 0);
    }
    check(seen == 3, "they are the two different services' answers");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    if (!pub_sub::SessionManager::getOrCreate())
    {
        SPDLOG_ERROR("No zenoh session available; cannot run.");
        return 1;
    }

    test_data_round_trips_through_a_real_service();
    test_a_bad_request_is_never_sent();
    test_nobody_home_is_no_reply();
    test_a_throwing_handler_answers_with_its_reason();
    test_every_responder_is_collected();

    pub_sub::SessionManager::shutdown();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }
    SPDLOG_INFO("all dynamic service call checks passed");
    return 0;
}
