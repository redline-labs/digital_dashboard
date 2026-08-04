#include "agent_control/zenoh_methods.h"

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/topic_discovery.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>

#include <spdlog/spdlog.h>
#include <zenoh.hxx>

#include <string>
#include <vector>

namespace agent_control
{

namespace
{

// Everything the caller needs to fix a bad schema name themselves.
json knownSchemas()
{
    json out = json::array();
    for (const auto& name : pub_sub::get_available_schemas())
    {
        out.push_back(std::string(name));
    }
    return out;
}

std::expected<capnp::Schema, AgentError> schemaByName(const std::string& name)
{
    capnp::Schema schema = pub_sub::get_schema(name);
    if (schema.getProto().getId() == 0)
    {
        json data = json::object();
        data["schema"] = name;
        data["known_schemas"] = knownSchemas();
        return std::unexpected(AgentError{ErrorCode::kBadParams,
                                          "Unknown schema '" + name + "'.",
                                          std::move(data)});
    }
    return schema;
}

std::expected<std::string, AgentError> requiredString(const json& params, const char* key)
{
    if (!params.contains(key) || !params[key].is_string())
    {
        return std::unexpected(
            badParams(std::string("'") + key + "' is required and must be a string."));
    }
    return params[key].get<std::string>();
}

int optIntOr(const json& params, const char* key, int fallback)
{
    if (params.contains(key) && params[key].is_number_integer())
    {
        return params[key].get<int>();
    }
    return fallback;
}

}  // namespace

void registerZenohMethods(AgentServer& server)
{
    // ------------------------------------------------------------ zenoh.list
    server.registerMethod(
        "zenoh.list",
        [](const json& params) -> MethodResult
        {
            const std::string keyexpr =
                params.contains("key") && params["key"].is_string()
                    ? params["key"].get<std::string>()
                    : std::string("**");
            const int window_ms = optIntOr(params, "window_ms", 1000);

            const auto observed = pub_sub::observeTopics(keyexpr, window_ms);

            json topics = json::array();
            for (const auto& topic : observed)
            {
                json entry = json::object();
                entry["key"] = topic.key;
                entry["schema"] = topic.schema;
                entry["count"] = topic.count;
                entry["hz"] = topic.hz;
                topics.push_back(std::move(entry));
            }

            json out = json::object();
            out["key"] = keyexpr;
            out["window_ms"] = window_ms;
            out["topics"] = std::move(topics);
            if (observed.empty())
            {
                // Worth stating plainly: zenoh has no retained messages and no
                // registry, so discovery only sees what is published while we
                // are listening. An empty list is not proof of an empty bus.
                out["note"] =
                    "Nothing published on '" + keyexpr + "' during the window. Topics are "
                    "discovered by observing traffic (zenoh has no retained messages), so a "
                    "slow or idle publisher looks identical to one that does not exist. Try a "
                    "longer window_ms.";
            }
            return out;
        });

    // ------------------------------------------------------------ zenoh.read
    server.registerMethod(
        "zenoh.read",
        [](const json& params) -> MethodResult
        {
            const auto key = requiredString(params, "key");
            if (!key.has_value())
            {
                return std::unexpected(key.error());
            }
            const int timeout_ms = optIntOr(params, "timeout_ms", 2000);

            const auto sample = pub_sub::readOneSample(key.value(), timeout_ms);
            if (!sample.has_value())
            {
                json data = json::object();
                data["key"] = key.value();
                data["timeout_ms"] = timeout_ms;
                // The trap worth naming: zenoh has no retained messages. A
                // one-shot zenoh.publish followed by a zenoh.read never works,
                // because the read subscribes after the sample is already gone.
                // This project has been bitten by the same property before (the
                // CarPlay video-config black screen), and without saying so the
                // failure looks like a broken publish.
                return std::unexpected(AgentError{
                    ErrorCode::kTimeout,
                    "No sample on '" + key.value() + "' within " + std::to_string(timeout_ms) +
                        " ms. Either nothing publishes it, or it is published too slowly for "
                        "this window. Note that zenoh has no retained messages: reading back a "
                        "value you published once will always time out, because the read "
                        "subscribes after that sample is gone. Read from a continuous "
                        "publisher instead.",
                    std::move(data)});
            }

            json out = json::object();
            out["key"] = sample->key;
            out["schema"] = sample->schema;
            out["bytes"] = sample->payload.size();

            if (sample->schema.empty())
            {
                out["note"] =
                    "The sample carries no capnp schema in its encoding, so it cannot be "
                    "decoded. Its publisher is not using pub_sub::ZenohPublisher.";
                return out;
            }

            auto schema = schemaByName(sample->schema);
            if (!schema.has_value())
            {
                return std::unexpected(schema.error());
            }

            try
            {
                out["value"] = pub_sub::capnpToJson(sample->payload, schema.value());
            }
            catch (const kj::Exception& e)
            {
                // capnp reports structural damage by throwing, and a truncated
                // or corrupt payload is exactly what an agent poking at a live
                // bus will eventually hit.
                return std::unexpected(internalError(
                    std::string("Failed to decode the sample: ") + e.getDescription().cStr()));
            }

            return out;
        });

    // --------------------------------------------------------- zenoh.publish
    server.registerMethod(
        "zenoh.publish",
        [](const json& params) -> MethodResult
        {
            const auto key = requiredString(params, "key");
            if (!key.has_value())
            {
                return std::unexpected(key.error());
            }
            const auto schema_name = requiredString(params, "schema");
            if (!schema_name.has_value())
            {
                return std::unexpected(schema_name.error());
            }
            if (!params.contains("fields") || !params["fields"].is_object())
            {
                return std::unexpected(
                    badParams("'fields' must be an object of the message's fields. Use "
                              "zenoh.describe_schema to see what they are."));
            }

            auto schema = schemaByName(schema_name.value());
            if (!schema.has_value())
            {
                return std::unexpected(schema.error());
            }

            capnp::MallocMessageBuilder message;
            auto root = message.initRoot<capnp::DynamicStruct>(schema.value().asStruct());

            std::vector<std::string> errors;
            if (!pub_sub::jsonToCapnp(params["fields"], root, errors))
            {
                json problems = json::array();
                for (const auto& error : errors)
                {
                    problems.push_back(error);
                }
                json data = json::object();
                data["problems"] = std::move(problems);
                return std::unexpected(AgentError{ErrorCode::kBadParams,
                                                  "Message was rejected; nothing was published.",
                                                  std::move(data)});
            }

            auto session = pub_sub::SessionManager::getOrCreate();
            if (!session)
            {
                return std::unexpected(
                    internalError("Failed to obtain a zenoh session."));
            }

            try
            {
                const auto words = capnp::messageToFlatArray(message);
                const auto bytes = words.asBytes();

                // Stamp the encoding exactly as ZenohPublisher does. A sample
                // published without it, or with the wrong schema name, decodes
                // against the wrong struct on the far side -- and capnp does not
                // error on that, it just reads different bytes and yields a
                // plausible wrong number. Subscribers check this field.
                zenoh::Session::PutOptions options;
                options.encoding.emplace(pub_sub::kCapnpEncodingMime);
                options.encoding->set_schema(schema_name.value());

                session->put(zenoh::KeyExpr(key.value()),
                             zenoh::Bytes(std::vector<uint8_t>(bytes.begin(), bytes.end())),
                             std::move(options));

                json out = json::object();
                out["key"] = key.value();
                out["schema"] = schema_name.value();
                out["bytes"] = bytes.size();
                out["published"] = true;
                return out;
            }
            catch (const std::exception& e)
            {
                return std::unexpected(
                    internalError(std::string("Publish failed: ") + e.what()));
            }
        },
        AgentServer::MethodKind::kMutating);

    // ------------------------------------------------------------ zenoh.rate
    server.registerMethod(
        "zenoh.rate",
        [](const json& params) -> MethodResult
        {
            const auto key = requiredString(params, "key");
            if (!key.has_value())
            {
                return std::unexpected(key.error());
            }
            const int window_ms = optIntOr(params, "window_ms", 1000);

            const auto observed = pub_sub::observeTopics(key.value(), window_ms);

            json topics = json::array();
            std::uint64_t total = 0;
            for (const auto& topic : observed)
            {
                json entry = json::object();
                entry["key"] = topic.key;
                entry["count"] = topic.count;
                entry["hz"] = topic.hz;
                topics.push_back(std::move(entry));
                total += topic.count;
            }

            json out = json::object();
            out["key"] = key.value();
            out["window_ms"] = window_ms;
            out["total"] = total;
            out["hz"] = (window_ms > 0)
                            ? static_cast<double>(total) * 1000.0 / static_cast<double>(window_ms)
                            : 0.0;
            out["topics"] = std::move(topics);
            return out;
        });

    // ------------------------------------------------- zenoh.describe_schema
    server.registerMethod(
        "zenoh.describe_schema",
        [](const json& params) -> MethodResult
        {
            if (!params.contains("schema"))
            {
                json out = json::object();
                out["schemas"] = knownSchemas();
                return out;
            }

            const auto schema_name = requiredString(params, "schema");
            if (!schema_name.has_value())
            {
                return std::unexpected(schema_name.error());
            }
            auto schema = schemaByName(schema_name.value());
            if (!schema.has_value())
            {
                return std::unexpected(schema.error());
            }

            json out = pub_sub::describeSchema(schema.value());
            out["schema"] = schema_name.value();
            return out;
        });
}

}  // namespace agent_control
