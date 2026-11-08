// The JSON-RPC envelope as a headless process gets it: no Qt, no socket, and an
// invoker standing in for "run this on my own thread".
//
// test_framing pins the same envelope through AgentServer; this pins that the
// envelope lives in the Dispatcher and that an invoker sees exactly the calls
// that passed it -- a malformed request must never reach a simulator's run
// loop, and a method's error must come back with its own reason.

#include "agent_control/dispatcher.h"
#include "agent_control/log_ring.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using agent_control::json;

json call(agent_control::Dispatcher& dispatcher, const std::string& line)
{
    return json::parse(dispatcher.handleLine(line), nullptr, false);
}

std::string reasonOf(const json& response)
{
    if (!response.is_object() || !response.contains("error"))
    {
        return "<no error>";
    }
    return response["error"]["data"].value("reason", "<none>");
}

}  // namespace

int main()
{
    int invoked = 0;
    int last_timeout = 0;
    agent_control::Dispatcher dispatcher(
        "test",
        [&](const std::string&, const agent_control::Dispatcher::Entry& entry, const json& params, int timeout_ms)
        {
            ++invoked;
            last_timeout = timeout_ms;
            return agent_control::Dispatcher::runHandler(entry, params);
        });

    dispatcher.registerMethod(
        "test.echo", [](const json& params) -> agent_control::MethodResult { return json{{"got", params}}; },
        agent_control::MethodKind::kReadOnly, "echoes its params");
    dispatcher.registerMethod("test.boom",
                              [](const json&) -> agent_control::MethodResult
                              { throw std::runtime_error("deliberate"); });
    dispatcher.registerMethod("test.mode",
                              [](const json&) -> agent_control::MethodResult
                              {
                                  return std::unexpected(agent_control::AgentError{
                                      agent_control::ErrorCode::kWrongMode, "stepped", json::object()});
                              });

    {
        const json r = call(dispatcher, R"({"jsonrpc":"2.0","id":"a","method":"test.echo","params":{"x":1}})");
        check(r["id"] == "a" && r["result"]["got"]["x"] == 1, "a well-formed call round-trips with its id");
        check(invoked == 1, "a well-formed call goes through the invoker");
        check(last_timeout == agent_control::Dispatcher::kDefaultTimeoutMs, "the default timeout reaches the invoker");
    }

    // Nothing malformed reaches the invoker.
    const int before = invoked;
    check(reasonOf(call(dispatcher, "{not json")) == "BAD_PARAMS", "unparseable input is BAD_PARAMS");
    check(reasonOf(call(dispatcher, "42")) == "BAD_PARAMS", "a non-object request is BAD_PARAMS");
    check(reasonOf(call(dispatcher, R"({"id":3})")) == "BAD_PARAMS", "a missing method is BAD_PARAMS");
    check(call(dispatcher, R"({"id":3})")["id"] == 3, "the id is echoed on an envelope error");
    check(reasonOf(call(dispatcher, R"({"method":"test.echo","params":[1]})")) == "BAD_PARAMS",
          "array params are BAD_PARAMS");
    check(reasonOf(call(dispatcher, R"({"method":"test.echo","params":null})")) == "BAD_PARAMS",
          "null params are BAD_PARAMS");
    check(reasonOf(call(dispatcher, R"({"method":"test.echo","params":{"_timeout_ms":-5}})")) == "BAD_PARAMS",
          "a negative _timeout_ms is BAD_PARAMS");
    check(reasonOf(call(dispatcher, R"({"method":"no.such"})")) == "NO_SUCH_METHOD", "an unknown method");
    check(invoked == before, "no malformed request reached the invoker");

    call(dispatcher, R"({"method":"test.echo","params":{"_timeout_ms":250}})");
    check(last_timeout == 250, "_timeout_ms reaches the invoker");

    {
        const json r = call(dispatcher, R"({"method":"test.boom"})");
        check(reasonOf(r) == "INTERNAL", "a throwing handler is INTERNAL, not an escaped exception");
    }
    {
        const json r = call(dispatcher, R"({"method":"test.mode"})");
        check(reasonOf(r) == "WRONG_MODE", "a handler's own reason survives");
        check(r["error"]["code"] == 1010, "WRONG_MODE has its own code");
    }
    {
        const json r = call(dispatcher, R"({"method":"rpc.methods"})");
        check(r["result"]["app"] == "test", "rpc.methods names the app");
        check(r["result"]["summaries"]["test.echo"] == "echoes its params", "summaries are listed");
    }

    // No invoker: runs inline.
    {
        agent_control::Dispatcher inline_dispatcher("inline");
        inline_dispatcher.registerMethod("x", [](const json&) -> agent_control::MethodResult { return 7; });
        check(call(inline_dispatcher, R"({"id":1,"method":"x"})")["result"] == 7, "no invoker runs inline");
    }

    // app.logs without Qt.
    {
        check(reasonOf(json{{"error", agent_control::queryLogs(json::object()).error().toJson()}}) ==
                  "UNSUPPORTED_IN_APP",
              "app.logs before the ring is installed says so");
        agent_control::installLogRing(8);
        SPDLOG_INFO("marker-{}", 42);
        const auto logs = agent_control::queryLogs(json{{"grep", "marker-42"}});
        check(logs.has_value() && logs->at("records").size() == 1, "app.logs finds a line logged after install");
        check(!agent_control::queryLogs(json{{"limit", -1}}).has_value(), "a negative limit is refused");
        check(!agent_control::queryLogs(json{{"level", "loud"}}).has_value(), "an unknown level is refused");
    }

    std::printf("agent_control_test_dispatcher: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
