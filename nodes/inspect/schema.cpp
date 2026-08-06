#include "inspect/verbs.h"

#include "inspect/describe.h"

#include "cli/output.h"

#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_registry.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

namespace inspect
{

void addSchemaOptions(cxxopts::Options& options)
{
    options.add_options()
        ("name", "Schema to describe. Omit to list them all.", cxxopts::value<std::string>())
        ("f,filter", "Only list schemas whose name contains this (case-insensitive).",
            cxxopts::value<std::string>());

    options.parse_positional({"name"});
}

int runSchema(cli::Context& context)
{
    // NO BUS.
    //
    // The registry is generated at build time from schemas/*.capnp, so this verb
    // answers entirely from the binary. It is the only inspect verb that works
    // with no zenoh session at all -- which makes it the right thing to reach
    // for when you are writing a `publish` or `call` invocation and need to know
    // what fields a message has, before anything is running.

    if (!context.has("name"))
    {
        std::vector<std::string> names;
        for (const auto& name : pub_sub::get_available_schemas())
        {
            names.emplace_back(name);
        }
        std::sort(names.begin(), names.end());

        if (context.has("filter"))
        {
            const std::string filter = context.stringOr("filter", "");
            std::string lowered_filter;
            std::transform(filter.begin(), filter.end(), std::back_inserter(lowered_filter),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            names.erase(std::remove_if(names.begin(), names.end(),
                                       [&](const std::string& name)
                                       {
                                           std::string lowered;
                                           std::transform(
                                               name.begin(), name.end(),
                                               std::back_inserter(lowered), [](unsigned char c) {
                                                   return static_cast<char>(std::tolower(c));
                                               });
                                           return lowered.find(lowered_filter) ==
                                                  std::string::npos;
                                       }),
                        names.end());
        }

        if (context.json())
        {
            cli::out("{}", nlohmann::json(names).dump(2));
            return cli::kOk;
        }

        for (const std::string& name : names)
        {
            cli::out("{}", name);
        }
        cli::out("");
        cli::out("{} schema(s). `inspect schema <Name>` describes one.", names.size());
        return cli::kOk;
    }

    const std::string name = context.stringOr("name", "");
    const auto schema = pub_sub::get_schema(name);
    if (!schema)
    {
        SPDLOG_ERROR("'{}' is not a schema in this build's registry.", name);
        SPDLOG_INFO("`inspect schema` with no argument lists them all.");
        return cli::kUsage;
    }

    const nlohmann::json described = pub_sub::describeSchema(*schema);

    if (context.json())
    {
        cli::out("{}", described.dump(2));
        return cli::kOk;
    }

    cli::out("{}", name);
    printSchemaFields(described, "  ");

    return cli::kOk;
}

}  // namespace inspect
