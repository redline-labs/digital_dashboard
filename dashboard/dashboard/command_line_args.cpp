#include "dashboard/command_line_args.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <iostream>

#include <unistd.h>

std::string default_mcp_socket_path()
{
    return "/tmp/redline_agent_" + std::to_string(::getpid()) + ".sock";
}

std::optional<CommandLineArgs> parse_command_line_args(int argc, char** argv)
{
    try
    {
        cxxopts::Options options("dashboard", "Vehicle instrument cluster.");
        
        options.add_options("required")
            ("c,config", "Path to YAML configuration file.", cxxopts::value<std::string>());
        
        options.add_options("optional")
            ("debug", "Enable debug logging.", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("mcp", "Enable the agent control interface on a unix socket, and run headless "
                    "(forces the Qt platform to 'offscreen'). Defaults to /tmp/redline_agent_<pid>.sock.",
                cxxopts::value<std::string>()->implicit_value(""))
            ("h,help", "Print usage");
        
        auto args_result = options.parse(argc, argv);

        // Handle help request
        if (args_result.count("help") != 0)
        {
            std::cout << options.help({"required", "optional"}) << std::endl;
            return std::nullopt;  // Indicate help was shown
        }

        // Refuse leftovers rather than ignoring them. The trap this catches:
        // cxxopts options with an implicit value do NOT consume a
        // space-separated argument, so `--mcp /tmp/a.sock` silently enables the
        // interface on the *default* path and drops the one you asked for. A
        // control socket listening somewhere other than where the caller was
        // told is not a failure mode worth tolerating.
        if (!args_result.unmatched().empty())
        {
            for (const auto& leftover : args_result.unmatched())
            {
                SPDLOG_CRITICAL("Unrecognised argument '{}'.", leftover);
            }
            SPDLOG_CRITICAL("Note: --mcp takes its value with '=', as in "
                            "--mcp=/tmp/agent.sock. Bare --mcp uses the default path.");
            return std::nullopt;
        }

        // Check for required config option
        if (args_result.count("config") == 0)
        {
            SPDLOG_CRITICAL("No configuration file specified. Use --config <file>");
            return std::nullopt;
        }

        // Build and return the parsed arguments
        CommandLineArgs parsed_args;
        parsed_args.config_file_path = args_result["config"].as<std::string>();
        parsed_args.debug_enabled = args_result["debug"].as<bool>();
        parsed_args.help_requested = false;  // We already handled help above

        if (args_result.count("mcp") != 0)
        {
            // implicit_value("") means "--mcp with no argument"; fill in the
            // pid-derived default so the socket path is always concrete from
            // here on and nothing downstream has to re-derive it.
            std::string path = args_result["mcp"].as<std::string>();
            if (path.empty())
            {
                path = default_mcp_socket_path();
            }
            parsed_args.mcp_socket_path = path;
        }

        return parsed_args;
    }
    catch (const cxxopts::exceptions::specification& e)
    {
        SPDLOG_CRITICAL("Failed to parse command line arguments: (cxxopts::specification : {})", e.what());
        return std::nullopt;
    }
    catch (const cxxopts::exceptions::parsing& e)
    {
        SPDLOG_CRITICAL("Failed to parse command line arguments: (cxxopts::parsing : {})", e.what());
        return std::nullopt;
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        SPDLOG_CRITICAL("Failed to parse command line arguments: (cxxopts::exception : {})", e.what());
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