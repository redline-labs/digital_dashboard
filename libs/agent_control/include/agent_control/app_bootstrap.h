#ifndef AGENT_CONTROL_APP_BOOTSTRAP_H_
#define AGENT_CONTROL_APP_BOOTSTRAP_H_

#include <optional>
#include <string>
#include <string_view>

#include <cxxopts.hpp>

namespace agent_control
{

class AgentServer;

// What every GUI app does to be driven by an agent, said once: the dashboard,
// the editor, scope and switchboard each carried their own copy of the --mcp
// option, the leftover-argument check, the headless setup and the readiness
// handshake.

// Declares `--mcp[=<socket>]` on `options`. Bare --mcp listens on
// /tmp/redline_<app>_<pid>.sock.
void addMcpOption(cxxopts::Options& options, std::string_view app);

// The socket to listen on when --mcp was given, defaulted; nullopt otherwise.
std::optional<std::string> mcpSocketPath(const cxxopts::ParseResult& result, std::string_view app);

// Refuses arguments cxxopts did not consume, logging each, and returns false.
//
// The trap this catches: an option with an implicit value does NOT take a
// space-separated argument, so `--mcp /tmp/a.sock` would listen on the default
// path and drop the one asked for. A control socket somewhere other than where
// the caller was told is not a failure mode worth tolerating.
bool rejectUnmatched(const cxxopts::ParseResult& result);

// Before QApplication is constructed -- the platform plugin is chosen then and
// cannot change afterwards: forces the offscreen platform (no display, no
// window stealing focus) and installs the log ring behind app.logs.
void prepareHeadless();

// Starts `server` on `socket_path` and prints the readiness line a supervisor
// waits for. False, logged, if the socket would not open.
//
// The line rather than polling for the socket file: the socket exists from the
// moment bind() returns, which is before the window is up, so a poller would
// connect too early and see an empty widget tree.
bool startAndAnnounce(AgentServer& server, const std::string& socket_path);

}  // namespace agent_control

#endif  // AGENT_CONTROL_APP_BOOTSTRAP_H_
