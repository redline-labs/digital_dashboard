#include "inspect/verbs.h"

#include "cli/output.h"

#include "pub_sub/dynamic_service_call.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/topic_directory.h"

#include <zenoh.hxx>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace inspect
{

void addServicesOptions(cxxopts::Options& options)
{
    options.add_options()
        ("all", "Include services whose node has gone away.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"));
}

int runServices(cli::Context& context)
{
    // Services were undiscoverable before this.
    //
    // pub_sub::ZenohService declares a zenoh queryable; zenoh will route a
    // request to it, but nothing on the bus said it existed, what key to address
    // it at, or what a request should contain. The only way to find out was to
    // read the source of whichever node happened to offer it.
    pub_sub::ServiceDirectory services;
    pub_sub::NodeDirectory nodes;

    if (!services.isValid())
    {
        SPDLOG_ERROR("Could not watch the service space; is a zenoh session available?");
        return cli::kFailure;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::vector<pub_sub::ServiceEntry> entries;
    for (const pub_sub::ServiceEntry& entry : services.snapshot())
    {
        if (entry.reachable || context.flag("all"))
        {
            entries.push_back(entry);
        }
    }

    if (context.json())
    {
        nlohmann::json out = nlohmann::json::array();
        for (const pub_sub::ServiceEntry& entry : entries)
        {
            nlohmann::json row;
            row["key"] = entry.key;
            row["request_schema"] = entry.request_schema;
            row["response_schema"] = entry.response_schema;
            row["reachable"] = entry.reachable;
            row["owner_zid"] = entry.owner_zid;
            row["owner"] = nodes.nameFor(entry.owner_zid);
            out.push_back(std::move(row));
        }
        cli::out("{}", out.dump(2));
        return cli::kOk;
    }

    if (entries.empty())
    {
        cli::out("No services are advertised.");
        return cli::kOk;
    }

    for (const pub_sub::ServiceEntry& entry : entries)
    {
        const std::string owner = nodes.nameFor(entry.owner_zid);
        cli::out("{}{}", entry.key, entry.reachable ? "" : "  [unreachable]");
        cli::out("  request   {}", entry.request_schema);
        cli::out("  response  {}", entry.response_schema);
        if (!owner.empty())
        {
            cli::out("  offered by {}", owner);
        }
        cli::out("");
    }

    cli::out("`inspect call <key> --data '{{...}}'` calls one; `inspect schema <Name>` shows "
             "what a request needs.");

    return cli::kOk;
}

void addCallOptions(cxxopts::Options& options)
{
    options.add_options()
        ("k,key", "Service to call.", cxxopts::value<std::string>())
        ("d,data", "Request fields as a JSON object, or '-' for stdin, or @file.",
            cxxopts::value<std::string>()->default_value("{}"))
        ("s,schema", "Request schema. Taken from the advertisement when omitted.",
            cxxopts::value<std::string>())
        ("t,timeout", "How long to wait for a reply, in milliseconds.",
            cxxopts::value<std::uint64_t>()->default_value("2000"));

    options.parse_positional({"key"});
}

int runCall(cli::Context& context)
{
    const auto key = context.requireString("key");
    if (!key)
    {
        return cli::kUsage;
    }

    std::string data_text = context.stringOr("data", "{}");
    if (data_text == "-")
    {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        data_text = buffer.str();
    }
    else if (!data_text.empty() && data_text.front() == '@')
    {
        std::ifstream file(data_text.substr(1));
        if (!file)
        {
            SPDLOG_ERROR("Could not read '{}'.", data_text.substr(1));
            return cli::kUsage;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        data_text = buffer.str();
    }

    nlohmann::json fields;
    try
    {
        fields = nlohmann::json::parse(data_text);
    }
    catch (const nlohmann::json::exception& e)
    {
        SPDLOG_ERROR("--data is not valid JSON: {}", e.what());
        return cli::kUsage;
    }

    // The request schema comes from the advertisement, which is the whole point
    // of advertising services: a caller should not have to know, or be told, what
    // type to send.
    std::string request_schema = context.stringOr("schema", "");
    std::string response_schema;

    if (request_schema.empty())
    {
        pub_sub::ServiceDirectory services;
        if (!services.isValid())
        {
            SPDLOG_ERROR("Could not watch the service space.");
            return cli::kFailure;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // One entry per offering node now, so a key served twice appears twice.
        // They normally agree on the schema; when they do not, there is no
        // right request to build, and picking one would send the other node
        // bytes it decodes as something else.
        for (const pub_sub::ServiceEntry& entry : services.snapshot())
        {
            if (entry.key != *key)
            {
                continue;
            }
            if (request_schema.empty())
            {
                request_schema = entry.request_schema;
                response_schema = entry.response_schema;
            }
            else if (entry.request_schema != request_schema)
            {
                SPDLOG_ERROR("'{}' is offered by more than one node, with different request "
                             "schemas ('{}' and '{}').",
                             *key, request_schema, entry.request_schema);
                SPDLOG_INFO("`inspect services` shows which node offers which. Pass --schema to "
                            "choose.");
                return cli::kUsage;
            }
        }

        if (request_schema.empty())
        {
            SPDLOG_ERROR("'{}' is not an advertised service, so its request schema is unknown.",
                         *key);
            SPDLOG_INFO("`inspect services` lists what is callable. Pass --schema to call an "
                        "unadvertised one anyway.");
            return cli::kUsage;
        }
    }

    auto session = pub_sub::SessionManager::getOrCreate();
    if (!session)
    {
        SPDLOG_ERROR("No zenoh session available.");
        return cli::kFailure;
    }

    // A session opened a moment ago may not be linked to anything yet. Opening no
    // longer waits out zenoh's scouting delay (see session_manager.cpp), and a
    // query sent before discovery finishes is routed nowhere: it completes at
    // once with no replies, which surfaced as "no reply within the timeout" after
    // a few hundred milliseconds against a service that was plainly there. The
    // service directory above opened a session too, but it has been released by
    // now, so this one is new. Wait briefly for a peer or a router; a bus with
    // nobody on it gets the same answer as before, only a second later.
    const auto link_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (session->get_peers_z_id().empty() && session->get_routers_z_id().empty() &&
           std::chrono::steady_clock::now() < link_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // The request build, the query and the reply decoding are the library's --
    // the same code switchboard calls through -- so the two cannot disagree
    // about what a request means or what came back.
    pub_sub::ServiceCallRequest request;
    request.key = *key;
    request.request_schema = request_schema;
    request.response_schema = response_schema;
    request.fields = std::move(fields);
    request.timeout = std::chrono::milliseconds(context.uintOr("timeout", 2000));

    const pub_sub::ServiceCallResult result = pub_sub::callServiceBlocking(std::move(request));

    using Status = pub_sub::ServiceCallResult::Status;
    if (result.status == Status::RequestRejected)
    {
        SPDLOG_ERROR("Request rejected; nothing was sent:");
        for (const std::string& error : result.errors)
        {
            SPDLOG_ERROR("  {}", error);
        }
        SPDLOG_INFO("`inspect schema {}` shows the fields it accepts.", request_schema);
        return cli::kUsage;
    }
    if (result.status == Status::Failed)
    {
        for (const std::string& error : result.errors)
        {
            SPDLOG_ERROR("Call failed: {}", error);
        }
        return cli::kFailure;
    }
    if (result.status == Status::NoReply)
    {
        SPDLOG_ERROR("No reply from '{}' within the timeout.", *key);
        return cli::kFailure;
    }

    nlohmann::json out = nlohmann::json::array();
    for (const pub_sub::ServiceReply& reply : result.replies)
    {
        if (reply.is_error)
        {
            // These used to be dropped without a word, so a service that
            // refused the request was indistinguishable from one that was not
            // there.
            SPDLOG_ERROR("Service error: {}", reply.error_text);
            continue;
        }
        out.push_back(reply.value);
    }

    if (out.empty())
    {
        return cli::kFailure;
    }

    // One reply is the normal case, so print the object rather than a
    // single-element array -- a caller piping into jq should not have to index.
    cli::out("{}", (out.size() == 1 ? out[0] : out).dump(2));
    return cli::kOk;
}

}  // namespace inspect
