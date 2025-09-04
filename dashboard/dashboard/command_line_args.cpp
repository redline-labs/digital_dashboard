#include "dashboard/command_line_args.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <iostream>

std::optional<CommandLineArgs> parse_command_line_args(int argc, char** argv)
{
    try
    {
        cxxopts::Options options("dashboard", "Vehicle instrument cluster.");
        
        options.add_options("required")
            ("c,config", "Path to YAML configuration file.", cxxopts::value<std::string>());
        
        options.add_options("optional")
            ("debug", "Enable debug logging.", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("h,help", "Print usage");
        
        auto args_result = options.parse(argc, argv);

        // Handle help request
        if (args_result.count("help") != 0)
        {
            std::cout << options.help({"required", "optional"}) << std::endl;
            return std::nullopt;  // Indicate help was shown
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