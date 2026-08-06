#include "inspect/verbs.h"

#include "cli/output.h"

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/capnp_json.h"
#include "pub_sub/capnp_payload.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/topic_directory.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>

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

        for (const pub_sub::ServiceEntry& entry : services.snapshot())
        {
            if (entry.key == *key)
            {
                request_schema = entry.request_schema;
                response_schema = entry.response_schema;
                break;
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

    const auto schema = pub_sub::get_schema(request_schema);
    if (!schema)
    {
        SPDLOG_ERROR("Request schema '{}' is not in this build's registry.", request_schema);
        return cli::kUsage;
    }

    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(schema->asStruct());

    std::vector<std::string> errors;
    if (!pub_sub::jsonToCapnp(fields, root, errors))
    {
        SPDLOG_ERROR("Request rejected; nothing was sent:");
        for (const std::string& error : errors)
        {
            SPDLOG_ERROR("  {}", error);
        }
        SPDLOG_INFO("`inspect schema {}` shows the fields it accepts.", request_schema);
        return cli::kUsage;
    }

    auto session = pub_sub::SessionManager::getOrCreate();
    if (!session)
    {
        SPDLOG_ERROR("No zenoh session available.");
        return cli::kFailure;
    }

    const auto words = capnp::messageToFlatArray(message);
    const auto bytes = words.asBytes();

    std::mutex reply_mutex;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> replies;
    std::atomic<bool> done{false};

    try
    {
        zenoh::Session::GetOptions options;
        options.timeout_ms = context.uintOr("timeout", 2000);
        options.payload = zenoh::Bytes(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));

        zenoh::Encoding encoding(pub_sub::kCapnpEncodingMime);
        encoding.set_schema(request_schema);
        options.encoding = std::move(encoding);

        session->get(
            zenoh::KeyExpr(*key), "",
            [&](const zenoh::Reply& reply)
            {
                try
                {
                    if (!reply.is_ok())
                    {
                        return;
                    }
                    const zenoh::Sample& sample = reply.get_ok();
                    const std::string sample_encoding = sample.get_encoding().as_string();
                    const std::lock_guard<std::mutex> guard(reply_mutex);
                    replies.emplace_back(
                        std::string(pub_sub::schemaNameFromEncoding(sample_encoding)),
                        sample.get_payload().as_vector());
                }
                catch (...)
                {
                    // Must not escape into zenoh's Rust frame.
                }
            },
            [&]() { done = true; }, std::move(options));
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Call failed: {}", e.what());
        return cli::kFailure;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(context.uintOr("timeout", 2000) + 500);
    while (!done && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const std::lock_guard<std::mutex> guard(reply_mutex);

    if (replies.empty())
    {
        SPDLOG_ERROR("No reply from '{}' within the timeout.", *key);
        return cli::kFailure;
    }

    nlohmann::json out = nlohmann::json::array();
    for (const auto& [reply_schema_name, payload] : replies)
    {
        // Prefer the schema the responder stamped; fall back to what the
        // advertisement said. They should agree, and if they do not, the
        // responder is authoritative -- it is describing the bytes it sent.
        const std::string effective =
            reply_schema_name.empty() ? response_schema : reply_schema_name;
        const auto reply_schema = pub_sub::get_schema(effective);

        if (!reply_schema)
        {
            SPDLOG_WARN("Reply schema '{}' is not in this build's registry.", effective);
            continue;
        }

        try
        {
            out.push_back(pub_sub::capnpToJson(payload, *reply_schema));
        }
        catch (const kj::Exception& e)
        {
            SPDLOG_ERROR("Could not decode the reply: {}", e.getDescription().cStr());
            return cli::kFailure;
        }
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
