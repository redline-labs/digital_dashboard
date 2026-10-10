#include "switchboard/command_line_args.h"

#include <cxxopts.hpp>
#include "agent_control/app_bootstrap.h"
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <iostream>

namespace switchboard
{
namespace
{


}  // namespace

std::optional<CommandLineArgs> parseCommandLineArgs(int argc, char** argv)
{
    try
    {
        cxxopts::Options options("switchboard", "Call the services advertised on the bus.");

        options.add_options("optional")
            ("t,timeout", "How long a call waits for a reply, in milliseconds. Changeable in "
                          "the window.",
                cxxopts::value<int>()->default_value("2000"))
            ("debug", "Enable debug logging.",
                cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("h,help", "Print usage");
        agent_control::addMcpOption(options, "switchboard");

        auto result = options.parse(argc, argv);

        if (result.count("help") != 0)
        {
            std::cout << options.help({"optional"}) << std::endl;
            return std::nullopt;
        }

        if (!agent_control::rejectUnmatched(result))
        {
            return std::nullopt;
        }

        CommandLineArgs parsed;
        parsed.debug_enabled = result["debug"].as<bool>();
        parsed.timeout_ms = result["timeout"].as<int>();

        if (parsed.timeout_ms <= 0)
        {
            SPDLOG_CRITICAL("--timeout must be a positive number of milliseconds.");
            return std::nullopt;
        }

        parsed.mcp_socket_path = agent_control::mcpSocketPath(result, "switchboard");

        return parsed;
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        SPDLOG_CRITICAL("Failed to parse command line arguments: (cxxopts : {})", e.what());
        return std::nullopt;
    }
    catch (const std::exception& e)
    {
        SPDLOG_CRITICAL("Failed to parse command line arguments: (std::exception : {})", e.what());
        return std::nullopt;
    }
}

}  // namespace switchboard
