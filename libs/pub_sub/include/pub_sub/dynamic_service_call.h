#ifndef PUB_SUB_DYNAMIC_SERVICE_CALL_H_
#define PUB_SUB_DYNAMIC_SERVICE_CALL_H_

#include "pub_sub/capnp_json.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace pub_sub
{

// A service call where the request and response types are known only by NAME,
// at runtime -- which is what a caller has when the service came from a
// ServiceDirectory advertisement rather than from a header it was compiled
// against. The request is built from JSON and every reply is decoded to JSON.
//
// ZenohAsyncClient is the typed counterpart and deliberately first-reply-wins,
// which is right for a map widget and wrong for a tool: a key served by two
// nodes is a problem the person calling it should see. This collects EVERY
// reply until zenoh says the query is over.

struct ServiceReply
{
    // An error reply (zenoh reply_err), or a reply this build could not decode.
    // `error_text` says which; `value` is null.
    bool is_error = false;
    std::string error_text;

    // The schema the reply was decoded against: the one the responder stamped
    // on it, else the advertised response schema. They should agree; when they
    // do not, the responder is describing the bytes it sent and wins.
    std::string schema;
    json value;
};

struct ServiceCallRequest
{
    std::string key;
    std::string request_schema;

    // Used only for a reply that carries no schema of its own. Our services
    // always stamp one, so empty is normally fine.
    std::string response_schema;

    json fields = json::object();
    std::chrono::milliseconds timeout{2000};

    // How the replies are decoded -- a tool showing a reply to a person wants
    // Data as hex, which is off by default. See CapnpJsonOptions.
    CapnpJsonOptions decode_options;
};

struct ServiceCallResult
{
    enum class Status
    {
        // At least one reply came back. Some or all of them may be errors;
        // check each reply's is_error.
        Replied,

        // The query completed with nothing: nobody serves the key, or nobody
        // answered inside the timeout. zenoh cannot tell those apart.
        NoReply,

        // The request never left: an unknown schema, or fields that do not fit
        // it. `errors` lists every problem, as jsonToCapnp reports them.
        RequestRejected,

        // No session, or zenoh refused the query itself. `errors` says why.
        Failed,
    };

    Status status = Status::Failed;
    std::vector<ServiceReply> replies;
    std::vector<std::string> errors;

    // From just before the query was sent to when zenoh reported it finished --
    // so for a NoReply this is the timeout, and for a reply it includes the
    // wait for any further responders.
    std::chrono::milliseconds elapsed{0};
};

const char* to_string(ServiceCallResult::Status status);

// Sends one request. `done` is called EXACTLY ONCE:
//   * on a zenoh thread when the query was sent, once zenoh reports it over;
//   * on the calling thread, before this returns, when it was not sent
//     (RequestRejected, Failed) -- so a caller that only handles `done` still
//     handles every outcome.
// Returns true when the query was sent.
//
// `done` must not block and must not touch GUI objects directly; hop to the
// owning thread first.
bool callService(ServiceCallRequest request, std::function<void(ServiceCallResult)> done);

// The same, waiting for the result. For a CLI and for tests.
ServiceCallResult callServiceBlocking(ServiceCallRequest request);

}  // namespace pub_sub

#endif  // PUB_SUB_DYNAMIC_SERVICE_CALL_H_
