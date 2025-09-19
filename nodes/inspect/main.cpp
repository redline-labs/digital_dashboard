#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
#include <string>
#include <vector>

#include "inspect/dump.h"
#include "inspect/info.h"
#include "inspect/list.h"
#include "inspect/nodes.h"

int main(int argc, char** argv)
{
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    // Parse only the verb here; allow unrecognised options so per-verb flags can be parsed by handlers.
    cxxopts::Options options("inspect", "Zenoh inspector");
    options.allow_unrecognised_options();
    options.add_options()
        ("h,help", "Print usage")
        ("verb", "Verb to execute", cxxopts::value<std::string>())
        ("debug", "Enable debug logging", cxxopts::value<bool>()->default_value("false")->implicit_value("true"));

    options.parse_positional({"verb"});

    cxxopts::ParseResult parsed;
    try
    {
        parsed = options.parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        SPDLOG_INFO("Usage: inspect <verb> [options]\n  Verbs:\n    dump   Subscribe and print payloads\n    info   Show schema and publisher info");
        return 1;
    }

    if (parsed["debug"].as<bool>())
    {
        spdlog::set_level(spdlog::level::debug);
    }

    if (parsed.count("help") && parsed.count("verb") == 0)
    {
        SPDLOG_INFO(
            "Usage: inspect <verb> [options]\n"
            "\n"
            " --debug  Enable debug logging\n"
            "\n"
            "  Verbs:\n"
            "    dump   Subscribe and print payloads\n"
            "    info   Show schema and publisher info\n"
            "    list   List available keys (optional -k filter, default **)\n"
            "    nodes  List peers and routers in the system"
        );
        return 0;
    }

    const std::string verb = parsed["verb"].as<std::string>();

    if (verb == "dump")
    {
        return run_dump(argc, argv);
    }
    if (verb == "info")
    {
        return run_info(argc, argv);
    }
    if (verb == "list")
    {
        return run_list(argc, argv);
    }
    if (verb == "nodes")
    {
        return run_nodes(argc, argv);
    }

    SPDLOG_ERROR("Unknown verb: '{}'", verb);
    SPDLOG_INFO("Available verbs: dump, info");
    return 1;
}


