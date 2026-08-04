// Dispatcher and JSON-RPC envelope handling, driven through the real
// AgentServer::handleLine -- the same entry point the socket calls.
//
// The emphasis is on malformed and hostile input. Well-formed requests are the
// easy half and are exercised by every manual run; the failure modes that
// actually bite are a truncated line, a wrong-typed field, or a parameter name
// that does not exist, and those are worth pinning.

#include "agent_control/server.h"

#include <nlohmann/json.hpp>

#include <QCoreApplication>

#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using json = nlohmann::json;

json roundTrip(agent_control::AgentServer& server, const std::string& line)
{
    const std::string response = server.handleLine(line);
    if (response.empty())
    {
        return json::object();
    }
    return json::parse(response, nullptr, false);
}

std::string reasonOf(const json& response)
{
    if (!response.contains("error"))
    {
        return "<no error>";
    }
    return response["error"]["data"].value("reason", "<none>");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    agent_control::AgentServer server("test");

    server.registerMethod("test.echo",
                          [](const json& params) -> agent_control::MethodResult
                          {
                              json out = json::object();
                              out["got"] = params;
                              return out;
                          });

    server.registerMethod("test.boom",
                          [](const json&) -> agent_control::MethodResult
                          {
                              throw std::runtime_error("deliberate");
                          });

    // ---------------------------------------------------------- happy path
    {
        const json r = roundTrip(server, R"({"jsonrpc":"2.0","id":7,"method":"test.echo","params":{"a":1}})");
        check(r.contains("result"), "well-formed request produces a result");
        check(r["id"] == 7, "id is echoed back");
        check(r["jsonrpc"] == "2.0", "response declares jsonrpc 2.0");
        check(r["result"]["got"]["a"] == 1, "params reach the handler intact");
    }

    // ------------------------------------------------------- malformed input
    {
        const json r = roundTrip(server, "this is not json");
        check(r.contains("error"), "garbage input is an error, not a crash");
        check(reasonOf(r) == "BAD_PARAMS", "garbage input reports BAD_PARAMS");
        check(r["id"].is_null(), "an unparseable request still yields a null id");
    }
    {
        // Truncated JSON is the realistic version of the above: a client that
        // died mid-write, or a line longer than the socket's cap.
        const json r = roundTrip(server, R"({"jsonrpc":"2.0","id":1,"method":"test.ec)");
        check(r.contains("error"), "truncated JSON is an error");
    }
    {
        const json r = roundTrip(server, "[1,2,3]");
        check(reasonOf(r) == "BAD_PARAMS", "a non-object request is rejected");
    }
    {
        const json r = roundTrip(server, "{}");
        check(reasonOf(r) == "BAD_PARAMS", "a request with no method is rejected");
    }
    {
        const json r = roundTrip(server, R"({"method":123})");
        check(reasonOf(r) == "BAD_PARAMS", "a non-string method is rejected");
    }
    {
        const json r = roundTrip(server, R"({"method":"test.echo","params":"nope"})");
        check(reasonOf(r) == "BAD_PARAMS", "non-object params are rejected");
    }
    {
        const json r = roundTrip(server, R"({"method":"nope.nothing"})");
        check(reasonOf(r) == "NO_SUCH_METHOD", "an unknown method is reported as such");
    }

    // ------------------------------------------------------------- timeouts
    {
        const json r = roundTrip(server, R"({"method":"test.echo","params":{"_timeout_ms":"soon"}})");
        check(reasonOf(r) == "BAD_PARAMS", "a non-integer _timeout_ms is rejected");
    }
    {
        const json r = roundTrip(server, R"({"method":"test.echo","params":{"_timeout_ms":0}})");
        check(reasonOf(r) == "BAD_PARAMS", "a zero _timeout_ms is rejected");
    }

    // ------------------------------------------------- exceptions in handlers
    {
        // A handler that throws must become an error response. If this ever
        // regressed into an escaped exception it would take down the whole
        // application from a single bad tool call.
        const json r = roundTrip(server, R"({"method":"test.boom"})");
        check(reasonOf(r) == "INTERNAL", "a throwing handler yields INTERNAL");
        check(r["error"]["message"].get<std::string>().find("deliberate") != std::string::npos,
              "the exception message survives into the response");
    }

    // ------------------------------------------------------------- discovery
    {
        const json r = roundTrip(server, R"({"method":"rpc.methods"})");
        check(r.contains("result"), "rpc.methods answers");
        bool found = false;
        for (const auto& name : r["result"]["methods"])
        {
            if (name == "test.echo")
            {
                found = true;
            }
        }
        check(found, "a registered method appears in rpc.methods");
    }

    // ------------------------------------------------ responses are one line
    {
        // The wire format is newline-delimited, so an embedded newline in a
        // response would desynchronise the stream for every subsequent call.
        const std::string response =
            server.handleLine(R"({"method":"test.echo","params":{"s":"a\nb"}})");
        check(response.find('\n') == std::string::npos,
              "a response never contains a raw newline");
    }

    std::printf("%s: %d checks, %d failures\n", argv[0], g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
