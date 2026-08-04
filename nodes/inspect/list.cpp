#include "inspect/list.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
#include <zenoh.hxx>

#include <string>
#include <vector>

#include "pub_sub/topic_discovery.h"

// List available keys by issuing a query over a broad key expression and printing reply keyexprs.
int run_list(int argc, char** argv)
{
    cxxopts::Options options("inspect list", "List zenoh keys by querying the space");
    options.add_options()
        ("k,key", "Key expression filter", cxxopts::value<std::string>()->default_value("**"))
        ("t,timeout", "Query timeout ms", cxxopts::value<uint64_t>()->default_value("1000"))
        ("h,help", "Print usage");

    cxxopts::ParseResult result;
    try
    {
        result = options.parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        SPDLOG_INFO("{}", options.help());
        return 1;
    }

    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const std::string filter = result["key"].as<std::string>();
    const uint64_t timeout_ms = result["timeout"].as<uint64_t>();

    try {
        // Shared with the agent control interface's zenoh.list, so the two
        // cannot disagree about what is on the bus.
        const auto topics = pub_sub::observeTopics(filter, static_cast<int>(timeout_ms));

        SPDLOG_INFO("Found {} keys matching '{}'.", topics.size(), filter);
        for (const auto& topic : topics)
        {
            SPDLOG_INFO("{}  [{}]  {} msgs, {:.1f} Hz",
                        topic.key,
                        topic.schema.empty() ? "no schema" : topic.schema,
                        topic.count,
                        topic.hz);
        }

        return 0;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Error listing keys: {}", e.what());
        return 1;
    }
}


