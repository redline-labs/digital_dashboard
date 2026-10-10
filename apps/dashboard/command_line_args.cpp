#include "dashboard/command_line_args.h"

#include <cxxopts.hpp>
#include "agent_control/app_bootstrap.h"
#include <spdlog/spdlog.h>
#include <iostream>

#include <unistd.h>

std::optional<CommandLineArgs> parse_command_line_args(int argc, char** argv)
{
    try
    {
        cxxopts::Options options("dashboard", "Vehicle instrument cluster.");
        
        options.add_options("required")
            ("c,config", "Path to YAML configuration file.", cxxopts::value<std::string>());
        
        options.add_options("optional")
            ("debug", "Enable debug logging.", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("config-override", "A config on the data partition that replaces --config when present and valid; "
                                "rejected ones fall back to --config and are reported.",
                cxxopts::value<std::string>())
            ("check", "Validate --config (load it and build its windows headless) and exit 0 or 1.",
                cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("h,help", "Print usage");
        agent_control::addMcpOption(options, "dashboard");
        
        auto args_result = options.parse(argc, argv);

        // Handle help request
        if (args_result.count("help") != 0)
        {
            std::cout << options.help({"required", "optional"}) << std::endl;
            return std::nullopt;  // Indicate help was shown
        }

        if (!agent_control::rejectUnmatched(args_result))
        {
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
        parsed_args.check_only = args_result["check"].as<bool>();
        if (args_result.count("config-override") != 0)
        {
            parsed_args.config_override_path = args_result["config-override"].as<std::string>();
        }
        parsed_args.help_requested = false;  // We already handled help above

        parsed_args.mcp_socket_path = agent_control::mcpSocketPath(args_result, "dashboard");

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