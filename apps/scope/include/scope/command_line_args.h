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

    // A bag DIRECTORY to open. Empty means "offline with nothing loaded",
    // which is what a bare `scope` starts as.
    //
    // Applied after the workspace, so `--config w.yaml --bag drives/today`
    // opens the recording with the workspace's panels already bound to it.
    std::string bag_path;

    // Attach to the bus and start capturing at startup.
    //
    // Needed because the default is OFFLINE: without a flag there would be no
    // way to launch scope watching live data, which is what every existing
    // invocation did. Mutually exclusive with bag_path -- a bag is an offline
    // source, so asking for both asks for two different things.
    bool start_online = false;

    // Where per-user settings live. Empty means scope::settingsPath(), the
    // platform's config location.
    //
    // NOT a convenience. Without it, every ctest run and every MCP-driven run
    // would read and write the developer's real settings file -- a test that
    // adds a tileset would leave it there, and one that clears them would take
    // the user's away. Tests pass a path under their own temporary directory.
    std::string settings_path;

    bool debug_enabled = false;

    // Set only when --mcp was given; always a concrete path by then.
    std::optional<std::string> mcp_socket_path;
};

// Returns nullopt when parsing failed or --help was printed; the caller should
// exit non-zero either way.
std::optional<CommandLineArgs> parseCommandLineArgs(int argc, char** argv);

}  // namespace scope

#endif  // SCOPE_COMMAND_LINE_ARGS_H_
