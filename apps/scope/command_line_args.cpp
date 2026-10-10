#include "scope/command_line_args.h"

#include <cxxopts.hpp>
#include "agent_control/app_bootstrap.h"
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <iostream>

namespace scope
{
namespace
{


}  // namespace

std::optional<CommandLineArgs> parseCommandLineArgs(int argc, char** argv)
{
    try
    {
        cxxopts::Options options("scope", "Live time-series visualizer.");

        options.add_options("optional")
            ("c,config", "Path to a YAML workspace file. Omit to start empty.",
                cxxopts::value<std::string>())
            // DECLARED, not sniffed out of the leftovers. parseCommandLineArgs
            // rejects unmatched arguments -- see the check below -- so an
            // undeclared option would abort the whole startup rather than being
            // ignored.
            ("b,bag", "Path to a bag DIRECTORY to open. Implies offline, which is the default.",
                cxxopts::value<std::string>())
            ("online", "Attach to the bus and start capturing at startup. Scope is OFFLINE by "
                       "default and does not open a zenoh session without this.",
                cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            // Takes a required value, so the implicit-value trap the
            // unmatched-argument check below exists for does not apply: this
            // one does consume `--settings /tmp/s.yaml`.
            ("settings", "Path to the per-user settings file. Omit to use the platform's "
                         "config location.",
                cxxopts::value<std::string>())
            ("debug", "Enable debug logging.",
                cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("h,help", "Print usage");
        agent_control::addMcpOption(options, "scope");

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

        if (result.count("config") != 0)
        {
            parsed.workspace_path = result["config"].as<std::string>();
        }

        if (result.count("bag") != 0)
        {
            parsed.bag_path = result["bag"].as<std::string>();
        }

        if (result.count("settings") != 0)
        {
            parsed.settings_path = result["settings"].as<std::string>();
        }

        parsed.start_online = result["online"].as<bool>();

        // Contradictory, and refused rather than resolved by precedence. Either
        // order silently throws away half of what was asked for: applying --bag
        // last leaves a window that never went online, and applying --online
        // last leaves one that discards the bag it was told to open. A startup
        // that did half the job is worse than one that says why it did none.
        if (parsed.start_online && !parsed.bag_path.empty())
        {
            SPDLOG_CRITICAL("--online and --bag are mutually exclusive: a bag is an offline "
                            "source. Start with --bag and go online from the toolbar, or use "
                            "scope.open_recording once online.");
            return std::nullopt;
        }

        parsed.mcp_socket_path = agent_control::mcpSocketPath(result, "scope");

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
