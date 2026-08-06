#ifndef SCOPE_COMMAND_LINE_ARGS_H_
#define SCOPE_COMMAND_LINE_ARGS_H_

#include <optional>
#include <string>

namespace scope
{

struct CommandLineArgs
{
    // Empty means "start with an empty workspace". Unlike the dashboard, which
    // has nothing to show without a config, scope is useful from a cold start:
    // you add panels and pick signals live.
    std::string workspace_path;

    // A bag DIRECTORY to review instead of tailing the bus. Empty means live.
    //
    // Applied after the workspace, so `--config w.yaml --bag drives/today`
    // opens the recording with the workspace's panels already bound to it.
    std::string bag_path;

    bool debug_enabled = false;

    // Set only when --mcp was given; always a concrete path by then.
    std::optional<std::string> mcp_socket_path;
};

// Returns nullopt when parsing failed or --help was printed; the caller should
// exit non-zero either way.
std::optional<CommandLineArgs> parseCommandLineArgs(int argc, char** argv);

}  // namespace scope

#endif  // SCOPE_COMMAND_LINE_ARGS_H_
