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

    /// Unix socket path for the agent control interface, set by --mcp.
    /// Present means the interface is enabled, which also forces the Qt platform
    /// to "offscreen" so the app runs completely headless.
    std::optional<std::string> mcp_socket_path;

    // --config-override: a config on the writable data partition that replaces
    // the shipped one when it is present and valid. See config_override.h.
    std::optional<std::string> config_override_path;

    // --check: validate --config (load it and build its windows headless), print
    // the verdict, exit 0 or 1. Nothing is shown and READY is never sent.
    bool check_only = false;
};

/// Default socket path when --mcp is given with no value. Includes the pid so
/// two apps (a dashboard and an editor) never collide.
std::string default_mcp_socket_path();

/**
 * @brief Parse command line arguments
 * 
 * @param argc Number of command line arguments
 * @param argv Array of command line argument strings
 * @return std::optional<CommandLineArgs> Parsed arguments if successful, std::nullopt if help was shown or parsing failed
 */
std::optional<CommandLineArgs> parse_command_line_args(int argc, char** argv); 

#endif // COMMAND_LINE_ARGS_H
