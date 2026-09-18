// What the console's routes make of a client's input, checked without a socket:
// a wrong-typed field is a 400 with the field named, never an exception, and a
// timeout is clamped rather than trusted.

#include "service_routes.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

namespace
{

using namespace web_console;
using nlohmann::json;
using std::chrono::milliseconds;

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

json validCall()
{
    return json{{"key", "nodes/fake_backlight/set_brightness"},
                {"request_schema", "SetBrightnessRequest"},
                {"response_schema", "SetBrightnessResponse"},
                {"fields", {{"level", 3}}}};
}

// Parses without letting anything escape: an exception here is exactly the
// bug, so it is caught and reported as a failure rather than aborting the run.
bool parses(const json& body, pub_sub::ServiceCallRequest& call, std::string& error)
{
    try
    {
        return parseCallRequest(body, call, error);
    }
    catch (const std::exception& e)
    {
        check(false, std::string("parseCallRequest threw: ") + e.what() + " on " + body.dump());
        return false;
    }
}

void testValidCall()
{
    pub_sub::ServiceCallRequest call;
    std::string error;
    check(parses(validCall(), call, error), "a well-formed call parses");
    check(call.key == "nodes/fake_backlight/set_brightness", "key is read");
    check(call.fields == json{{"level", 3}}, "fields are read");
    check(call.timeout == milliseconds(2000), "no timeout_ms keeps the default");
}

void testWrongTypesAreRefusedNotThrown()
{
    const auto refused = [](json body, const std::string& field, const std::string& what) {
        pub_sub::ServiceCallRequest call;
        std::string error;
        check(!parses(body, call, error), what + " is refused");
        check(error.find(field) != std::string::npos,
              what + " names the field (got '" + error + "')");
    };

    json body = validCall();
    body["key"] = 42;
    refused(body, "key", "a numeric key");

    body = validCall();
    body["request_schema"] = nullptr;
    refused(body, "request_schema", "a null request_schema");

    body = validCall();
    body.erase("response_schema");
    refused(body, "response_schema", "a missing response_schema");

    body = validCall();
    body["key"] = "";
    refused(body, "key", "an empty key");

    body = validCall();
    body["fields"] = json::array({1, 2});
    refused(body, "fields", "fields as an array");

    body = validCall();
    body["fields"] = "level=3";
    refused(body, "fields", "fields as a string");

    body = validCall();
    body["timeout_ms"] = "500";
    refused(body, "timeout_ms", "a string timeout_ms");

    body = validCall();
    body["timeout_ms"] = true;
    refused(body, "timeout_ms", "a boolean timeout_ms");

    refused(json::array({validCall()}), "object", "a body that is an array");
    refused(json("key"), "object", "a body that is a string");
}

void testTimeoutIsClamped()
{
    const auto timeoutFor = [](json value) {
        json body = validCall();
        body["timeout_ms"] = std::move(value);
        pub_sub::ServiceCallRequest call;
        std::string error;
        check(parses(body, call, error), "timeout_ms " + body["timeout_ms"].dump() + " parses");
        return call.timeout;
    };

    check(timeoutFor(500) == milliseconds(500), "a timeout inside the range is kept");
    check(timeoutFor(0) == kMinCallTimeout, "zero is raised to the floor");
    check(timeoutFor(-5) == kMinCallTimeout, "a negative timeout is raised to the floor");
    check(timeoutFor(600000) == kMaxCallTimeout, "ten minutes is cut to the ceiling");
    check(timeoutFor(1e300) == kMaxCallTimeout, "a huge double cannot overflow");
    check(timeoutFor(json(18446744073709551615ULL)) == kMaxCallTimeout,
          "nor can the largest unsigned");
    check(timeoutFor(250.7) == milliseconds(250), "a fractional timeout is truncated");
}

}  // namespace

int main()
{
    testValidCall();
    testWrongTypesAreRefusedNotThrown();
    testTimeoutIsClamped();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
