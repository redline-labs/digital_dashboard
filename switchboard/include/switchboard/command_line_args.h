#ifndef SWITCHBOARD_COMMAND_LINE_ARGS_H_
#define SWITCHBOARD_COMMAND_LINE_ARGS_H_

#include <optional>
#include <string>

namespace switchboard
{

struct CommandLineArgs
{
    bool debug_enabled = false;

    // What a new call waits for a reply, until changed in the window. Two
    // seconds is `inspect call`'s default, so the two tools agree.
    int timeout_ms = 2000;

    // Set only when --mcp was given; always a concrete path by then.
    std::optional<std::string> mcp_socket_path;
};

// Returns nullopt when parsing failed or --help was printed; the caller should
// exit non-zero either way.
std::optional<CommandLineArgs> parseCommandLineArgs(int argc, char** argv);

}  // namespace switchboard

#endif  // SWITCHBOARD_COMMAND_LINE_ARGS_H_
