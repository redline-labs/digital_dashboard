#ifndef COMMAND_LINE_ARGS_H
#define COMMAND_LINE_ARGS_H

#include <string>
#include <optional>

/**
 * @brief Holds the parsed command line arguments for the dashboard application
 */
struct CommandLineArgs
{
    std::string config_file_path;  ///< Path to the YAML configuration file
    bool debug_enabled;            ///< Whether debug logging is enabled
    bool help_requested;           ///< Whether help was requested
};

/**
 * @brief Parse command line arguments
 * 
 * @param argc Number of command line arguments
 * @param argv Array of command line argument strings
 * @return std::optional<CommandLineArgs> Parsed arguments if successful, std::nullopt if help was shown or parsing failed
 */
std::optional<CommandLineArgs> parse_command_line_args(int argc, char** argv); 

#endif // COMMAND_LINE_ARGS_H
