#include "inspect/verbs.h"

#include "cli/interrupt.h"
#include "cli/output.h"

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/session_manager.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>

#include <zenoh.hxx>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace inspect
{

void addPublishOptions(cxxopts::Options& options)
{
    options.add_options()
        ("k,key", "Topic to publish on.", cxxopts::value<std::string>())
        ("s,schema", "Schema name. See `inspect schema`.", cxxopts::value<std::string>())
        ("d,data", "Message fields as a JSON object, or '-' to read stdin, or @file.",
            cxxopts::value<std::string>()->default_value("{}"))
        ("r,rate", "Republish this many times per second.", cxxopts::value<double>())
        ("n,count", "Publish this many messages (default 1).",
            cxxopts::value<std::uint64_t>()->default_value("1"));

    options.parse_positional({"key"});
}

int runPublish(cli::Context& context)
{
    const auto key = context.requireString("key");
    if (!key)
    {
        return cli::kUsage;
    }
    const auto schema_name = context.requireString("schema");
    if (!schema_name)
    {
        return cli::kUsage;
    }

    // Resolve the payload source before touching the bus, so a typo in the JSON
    // is reported without having opened a session or published anything.
    std::string data_text = context.stringOr("data", "{}");
    if (data_text == "-")
    {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        data_text = buffer.str();
    }
    else if (!data_text.empty() && data_text.front() == '@')
    {
        const std::string path = data_text.substr(1);
        std::ifstream file(path);
        if (!file)
        {
            SPDLOG_ERROR("Could not read '{}'.", path);
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

    if (!fields.is_object())
    {
        SPDLOG_ERROR("--data must be a JSON object of the message's fields.");
        return cli::kUsage;
    }

    const auto schema = pub_sub::get_schema(*schema_name);
    if (!schema)
    {
        SPDLOG_ERROR("'{}' is not a schema in this build's registry.", *schema_name);
        SPDLOG_INFO("`inspect schema` lists them; `inspect schema {}` would show its fields.",
                    *schema_name);
        return cli::kUsage;
    }

    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(schema->asStruct());

    // ALL OR NOTHING. An unknown field name is an error rather than something to
    // ignore, because a typo that publishes a default-valued message produces a
    // plausible wrong reading on a gauge -- far harder to notice than a
    // rejection. Same rule as jsonToCapnp's own contract.
    std::vector<std::string> errors;
    if (!pub_sub::jsonToCapnp(fields, root, errors))
    {
        SPDLOG_ERROR("Message rejected; nothing was published:");
        for (const std::string& error : errors)
        {
            SPDLOG_ERROR("  {}", error);
        }
        SPDLOG_INFO("`inspect schema {}` shows the fields it accepts.", *schema_name);
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

    // Stamp the encoding exactly as ZenohPublisher does. A sample published
    // without it, or with the wrong schema name, decodes against the wrong
    // struct on the far side -- and capnp does not error on that, it reads
    // different bytes and yields a plausible wrong number.
    const auto put = [&]()
    {
        zenoh::Session::PutOptions options;
        options.encoding.emplace(pub_sub::kCapnpEncodingMime);
        options.encoding->set_schema(*schema_name);
        session->put(zenoh::KeyExpr(*key),
                     zenoh::Bytes(std::vector<std::uint8_t>(bytes.begin(), bytes.end())),
                     std::move(options));
    };

    const std::uint64_t count = context.uintOr("count", 1);
    const double rate = context.doubleOr("rate", 0.0);

    if (rate < 0.0)
    {
        SPDLOG_ERROR("--rate must not be negative.");
        return cli::kUsage;
    }

    // A publisher that exits immediately can lose its own message: zenoh
    // discovery is asynchronous, and a subscriber that has not matched yet never
    // sees it. This is the single most common way a one-shot publish appears to
    // do nothing.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    cli::installInterruptHandler();

    std::uint64_t published = 0;
    const bool forever = count == 0;

    while (!cli::interrupted() && (forever || published < count))
    {
        try
        {
            put();
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Publish failed: {}", e.what());
            return cli::kFailure;
        }
        ++published;

        if (rate > 0.0 && (forever || published < count))
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / rate));
        }
        else if (rate == 0.0 && !forever && published >= count)
        {
            break;
        }
    }

    // Give the last message time to leave before the session closes.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (context.json())
    {
        nlohmann::json out;
        out["key"] = *key;
        out["schema"] = *schema_name;
        out["bytes"] = bytes.size();
        out["published"] = published;
        cli::out("{}", out.dump(2));
    }
    else
    {
        cli::out("Published {} message(s) of {} bytes on '{}' as {}.", published, bytes.size(),
                 *key, *schema_name);
    }

    return cli::kOk;
}

}  // namespace inspect
