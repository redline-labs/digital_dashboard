#include "switchboard/command_line_args.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <iostream>

namespace switchboard
{
namespace
{

std::string defaultMcpSocketPath()
{
    return "/tmp/redline_switchboard_" + std::to_string(::getpid()) + ".sock";
}

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
            ("mcp", "Enable the agent control interface on a unix socket, and run headless "
                    "(forces the Qt platform to 'offscreen'). Defaults to "
                    "/tmp/redline_switchboard_<pid>.sock.",
                cxxopts::value<std::string>()->implicit_value(""))
            ("h,help", "Print usage");

        auto result = options.parse(argc, argv);

        if (result.count("help") != 0)
        {
            std::cout << options.help({"optional"}) << std::endl;
            return std::nullopt;
        }

        // Refuse leftovers rather than ignoring them: cxxopts options with an
        // implicit value do NOT consume a space-separated argument, so
        // `--mcp /tmp/a.sock` would silently listen on the default path instead.
        // Same check, for the same reason, as scope and the dashboard.
        if (!result.unmatched().empty())
        {
            for (const auto& leftover : result.unmatched())
            {
                SPDLOG_CRITICAL("Unrecognised argument '{}'.", leftover);
            }
            SPDLOG_CRITICAL("Note: --mcp takes its value with '=', as in "
                            "--mcp=/tmp/agent.sock. Bare --mcp uses the default path.");
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

        if (result.count("mcp") != 0)
        {
            std::string path = result["mcp"].as<std::string>();
            if (path.empty())
            {
                path = defaultMcpSocketPath();
            }
            parsed.mcp_socket_path = path;
        }

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
