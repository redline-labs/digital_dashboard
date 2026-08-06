#include "cli/session_options.h"

#include "pub_sub/session_manager.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace cli
{

namespace
{

// JSON5 string literal. Endpoints and mode names come from the command line, so
// a quote or a backslash in one would produce a config fragment that does not
// parse -- buildConfig() catches that and logs it, but "ignoring zenoh config
// override" is a poor way to learn you typed a quote. Escape instead.
std::string jsonQuoted(const std::string& value)
{
    std::string quoted;
    quoted.reserve(value.size() + 2u);
    quoted.push_back('"');
    for (const char c : value)
    {
        if (c == '"' || c == '\\')
        {
            quoted.push_back('\\');
        }
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

}  // namespace

void applySessionOverrides(const cxxopts::ParseResult& parsed)
{
    if (parsed.count("mode") != 0)
    {
        const std::string mode = parsed["mode"].as<std::string>();

        // Checked here rather than left to zenoh: an unrecognised mode is
        // dropped by buildConfig() with a log line, and the session then opens
        // in the default peer mode -- so `--mode clietn` would connect to the
        // wrong thing and look like it worked.
        if (mode != "peer" && mode != "client" && mode != "router")
        {
            SPDLOG_WARN("Unrecognised --mode '{}'; expected peer, client or router. "
                        "Passing it through, but zenoh will most likely ignore it.", mode);
        }

        pub_sub::SessionManager::insertConfig("mode", jsonQuoted(mode));
    }

    if (parsed.count("connect") != 0)
    {
        const auto endpoints = parsed["connect"].as<std::vector<std::string>>();

        std::string json = "[";
        for (std::size_t i = 0; i < endpoints.size(); ++i)
        {
            if (i != 0u)
            {
                json += ",";
            }
            json += jsonQuoted(endpoints[i]);
        }
        json += "]";

        pub_sub::SessionManager::insertConfig("connect/endpoints", json);
    }
}

}  // namespace cli
