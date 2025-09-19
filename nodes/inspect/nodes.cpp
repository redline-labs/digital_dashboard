#include "inspect/nodes.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
#include <zenoh.hxx>

#include <string>

#include "pub_sub/session_manager.h"

int run_nodes(int argc, char** argv)
{
    cxxopts::Options options("inspect nodes", "List peers and routers in the system");
    options.add_options()
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

    try
    {
        auto session = pub_sub::SessionManager::getOrCreate();
        if (!session)
        {
            SPDLOG_ERROR("Failed to obtain zenoh session");
            return 1;
        }

        auto routers = session->get_routers_z_id();
        auto peers = session->get_peers_z_id();

        SPDLOG_INFO("Routers ({}):", routers.size());
        for (const auto& r : routers)
        {
            SPDLOG_INFO("  {}", r.to_string());
        }

        SPDLOG_INFO("Peers ({}):", peers.size());
        for (const auto& p : peers)
        {
            SPDLOG_INFO("  {}", p.to_string());
        }

#if defined(Z_FEATURE_UNSTABLE_API)
        // Try scouting to obtain locators (unstable API)
        SPDLOG_INFO("Scouting for detailed info (locators)...");
        zenoh::Config cfg = zenoh::Config::create_default();
        std::vector<zenoh::Id> seen;
        zenoh::scout(
            std::move(cfg),
            [&](const zenoh::Hello& hello)
            {
                auto id = hello.get_id();
                auto what = hello.get_whatami();
                auto locs = hello.get_locators();
                std::string role = (what == Z_WHATAMI_ROUTER ? "router" : (what == Z_WHATAMI_PEER ? "peer" : "other"));
                SPDLOG_INFO("{} {}:", role, id.to_string());
                for (auto sv : locs)
                {
                    SPDLOG_INFO("    locator: {}", std::string(sv));
                }
            },
            zenoh::closures::none
        );
#else
        SPDLOG_INFO("(Build without unstable API; locators not available)");
#endif

        return 0;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Error listing nodes: {}", e.what());
        return 1;
    }
}


