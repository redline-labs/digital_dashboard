#include "inspect/list.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
#include <zenoh.hxx>

#include <string>
#include <vector>

#include "pub_sub/session_manager.h"

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
        auto session = pub_sub::SessionManager::getOrCreate();
        if (!session)
        {
            SPDLOG_ERROR("Failed to obtain zenoh session");
            return 1;
        }

        // Subscribe to the provided keyexpr and collect keys seen during the time window.
        std::vector<std::string> keys;
        auto sub = session->declare_subscriber(
            zenoh::KeyExpr(filter),
            [&](const zenoh::Sample& sample)
            {
                keys.emplace_back(std::string(sample.get_keyexpr().as_string_view()));
            },
            zenoh::closures::none);

        // Collect for the timeout window
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));

        // Ensure subscriber is torn down before printing
        (void)sub;

        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

        SPDLOG_INFO("Found {} keys matching '{}'.", keys.size(), filter);
        for (const auto& k : keys)
        {
            SPDLOG_INFO("{}", k);
        }

        return 0;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Error listing keys: {}", e.what());
        return 1;
    }
}


