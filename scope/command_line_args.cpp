#include "scope/command_line_args.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <iostream>

namespace scope
{
namespace
{

std::string defaultMcpSocketPath()
{
    return "/tmp/redline_scope_" + std::to_string(::getpid()) + ".sock";
}

}  // namespace

std::optional<CommandLineArgs> parseCommandLineArgs(int argc, char** argv)
{
    try
    {
        cxxopts::Options options("scope", "Live time-series visualizer.");

        options.add_options("optional")
            ("c,config", "Path to a YAML workspace file. Omit to start empty.",
                cxxopts::value<std::string>())
            ("debug", "Enable debug logging.",
                cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("mcp", "Enable the agent control interface on a unix socket, and run headless "
                    "(forces the Qt platform to 'offscreen'). Defaults to /tmp/redline_scope_<pid>.sock.",
                cxxopts::value<std::string>()->implicit_value(""))
            ("h,help", "Print usage");

        auto result = options.parse(argc, argv);

        if (result.count("help") != 0)
        {
            std::cout << options.help({"optional"}) << std::endl;
            return std::nullopt;
        }

        // Refuse leftovers rather than ignoring them. The trap this catches:
        // cxxopts options with an implicit value do NOT consume a
        // space-separated argument, so `--mcp /tmp/a.sock` silently enables the
        // interface on the *default* path and drops the one you asked for. A
        // control socket listening somewhere other than where the caller was
        // told is not a failure mode worth tolerating, and it cost an afternoon
        // in the dashboard before the same check went in there.
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

        if (result.count("config") != 0)
        {
            parsed.workspace_path = result["config"].as<std::string>();
        }

        if (result.count("mcp") != 0)
        {
            // implicit_value("") means "--mcp with no argument"; fill in the
            // pid-derived default so the socket path is always concrete from
            // here on and nothing downstream has to re-derive it.
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
    catch (...)
    {
        SPDLOG_CRITICAL("Failed to parse command line arguments: (unknown exception)");
        return std::nullopt;
    }
}

}  // namespace scope
