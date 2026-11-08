#ifndef AGENT_CONTROL_MCP_OPTION_H_
#define AGENT_CONTROL_MCP_OPTION_H_

#include <optional>
#include <string>
#include <string_view>

#include <cxxopts.hpp>

namespace agent_control
{

// The Qt-free half of being driven by an agent: the --mcp option and the
// readiness line. GUI apps reach these through app_bootstrap.h; a headless
// process (a simulator) uses them directly.

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

// Prints the readiness line a supervisor waits for: `AGENT_READY <socket> <pid>`.
//
// The line rather than polling for the socket file: the socket exists from the
// moment bind() returns, which is before the process is ready to answer, so a
// poller would connect too early.
void announceReady(const std::string& socket_path);

}  // namespace agent_control

#endif  // AGENT_CONTROL_MCP_OPTION_H_
