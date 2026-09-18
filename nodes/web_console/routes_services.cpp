// Switchboard, in a browser.
//
//   GET  /api/services        what is offered, and by whom
//   GET  /api/schema/<name>   the request type's shape, for building a form
//   POST /api/call            call one, with JSON in and JSON out
//
// NOTHING HERE KNOWS ANY SERVICE. The directories are liveliness subscribers and
// the call goes through pub_sub::callServiceBlocking -- the same dynamic path
// apps/switchboard and `inspect call` use, JSON in and JSON out, with no
// compile-time knowledge of any Req/Resp type. A service added to the tree
// tomorrow appears here with no change to this file, which is the only reason a
// console can offer them at all.
//
// The node holds the session, as it does for health: one dynamic-call
// implementation rather than a second one reimplemented in wasm.

#include "service_routes.h"

#include "pub_sub/capnp_json.h"
#include "pub_sub/dynamic_service_call.h"
#include "pub_sub/schema_registry.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string>

namespace web_console
{
namespace
{

using nlohmann::json;

// A caller that has to know a service's key AND both schema names to invoke it
// is a caller that cannot be a generic form, so the listing carries all three.
json servicesJson(const ServiceRoutes& state)
{
    json list = json::array();
    for (const auto& service : state.services().snapshot())
    {
        list.push_back({
            {"key", service.key},
            {"request_schema", service.request_schema},
            {"response_schema", service.response_schema},
            {"owner_zid", service.owner_zid},
            // Which process offers it, when the node directory has caught up.
            {"owner", state.nodes().nameFor(service.owner_zid)},
            // Entries are never removed, only marked unreachable -- a service
            // that has gone is more useful to show than to hide.
            {"reachable", service.reachable},
            {"appearances", service.appearances},
            {"disappearances", service.disappearances},
        });
    }
    return list;
}

}  // namespace

void registerServiceRoutes(RouteRegistrar& routes, ServiceRoutes& state)
{
    routes.getReply("/api/services", [&state] {
        json out;
        out["bus_available"] = state.available();
        if (!state.available())
        {
            out["error"] = "no zenoh session; services cannot be enumerated";
            out["services"] = json::array();
            return jsonReply(200, out);
        }
        out["services"] = servicesJson(state);
        out["revision"] = state.services().revision();
        return jsonReply(200, out);
    });

    // The request type's shape, so the browser can build a form without knowing
    // the type. describeSchema() walks the real capnp schema; the doc comments
    // come from the generated tables, since capnp strips them from descriptors.
    routes.getReply("/api/schema", [] {
        json names = json::array();
        for (const auto& name : pub_sub::get_available_schemas())
        {
            names.push_back(std::string(name));
        }
        return jsonReply(200, json{{"schemas", names}});
    });

    // A GET, because describing a schema reads and changes nothing.
    routes.getWithParams("/api/schema/:name", [](const PathParams& params) {
        const auto found = params.find("name");
        const std::string name = found != params.end() ? found->second : std::string();
        if (name.empty())
        {
            return errorReply(400, "give a schema name");
        }

        const auto schema = pub_sub::get_schema(name);
        if (!schema)
        {
            return errorReply(404, "unknown schema: " + name);
        }

        json out;
        out["schema"] = name;
        out["fields"] = pub_sub::describeSchema(*schema);
        out["doc"] = std::string(pub_sub::schema_doc(schema->getProto().getId()));
        return jsonReply(200, out);
    });

    routes.post("/api/call", [&state](const std::string& body) {
        if (!state.available())
        {
            return errorReply(503, "no zenoh session; nothing can be called");
        }

        json request;
        try
        {
            request = json::parse(body);
        }
        catch (const json::exception& error)
        {
            return errorReply(400, std::string("body is not JSON: ") + error.what());
        }

        pub_sub::ServiceCallRequest call;
        call.key = request.value("key", "");
        call.request_schema = request.value("request_schema", "");
        call.response_schema = request.value("response_schema", "");
        call.fields = request.value("fields", json::object());
        call.timeout = std::chrono::milliseconds(request.value("timeout_ms", 2000));
        // Matches what switchboard passes: a Data field shows as hex rather than
        // an unbounded byte array.
        call.decode_options.data_hex_limit = 4096;

        if (call.key.empty() || call.request_schema.empty() || call.response_schema.empty())
        {
            return errorReply(400, "key, request_schema and response_schema are all required");
        }

        // Blocking, deliberately: this runs on an httplib worker thread, and an
        // HTTP request is exactly the shape of a blocking call. The async form
        // exists for the Qt app, which must not stall its event loop.
        const pub_sub::ServiceCallResult result = pub_sub::callServiceBlocking(call);

        json replies = json::array();
        for (const auto& reply : result.replies)
        {
            replies.push_back({
                {"is_error", reply.is_error},
                {"error_text", reply.error_text},
                {"schema", reply.schema},
                {"value", reply.value},
            });
        }

        json out{
            {"status", pub_sub::to_string(result.status)},
            {"replies", replies},
            {"errors", result.errors},
            {"elapsed_ms", result.elapsed.count()},
        };

        // Honest statuses rather than 200-with-an-error-field. Z_QUERY_TARGET_ALL
        // means several responders can answer, so Replied may carry more than one.
        switch (result.status)
        {
            case pub_sub::ServiceCallResult::Status::Replied:
                return jsonReply(200, out);
            case pub_sub::ServiceCallResult::Status::NoReply:
                // Nobody answered in time: the service may be gone, or slow.
                return jsonReply(504, out);
            case pub_sub::ServiceCallResult::Status::RequestRejected:
                // The fields did not fit the request schema -- jsonToCapnp's
                // errors say which, and they are already in `errors`.
                return jsonReply(400, out);
            case pub_sub::ServiceCallResult::Status::Failed:
            default:
                return jsonReply(502, out);
        }
    });
}

}  // namespace web_console
