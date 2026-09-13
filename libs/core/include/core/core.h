#ifndef CORE_H_
#define CORE_H_

#include <optional>
#include <string>
#include <string_view>

// The tree's runtime bring-up, shared by the dashboard, the tools and every
// node: how logs are formatted and where they go, where data and shipped
// resources live, and how a service tells systemd it is ready.
//
// Nothing here assumes a target. On a developer machine no REDLINE_* variable
// is set and every function falls back to something sensible for a checkout;
// on the LattePanda image the units set REDLINE_DATA_DIR=/data and systemd
// provides NOTIFY_SOCKET. docs/environment.md lists the variables.
namespace core
{

// ---- logging ----------------------------------------------------------------

struct LoggingOptions
{
    // Names the log file when one is written: <REDLINE_LOG_DIR>/<program>.txt.
    std::string program;
    // --debug / --verbose: debug level instead of info.
    bool debug = false;
    // Agent (--mcp) mode: stdout is a protocol channel, so logs go to stderr
    // only and no file is written. The caller installs its own capture after.
    bool stderr_only = false;
};

// The one place the tree's log format lives: pattern, level and sinks.
//
// The pattern is "[date time] [level] [thread:file:line] message", identical
// everywhere because logs from the dashboard, the nodes and the tools end up
// interleaved in the same terminal during bring-up.
//
// Sinks: the console, plus a rotating file <REDLINE_LOG_DIR>/<program>.txt
// when REDLINE_LOG_DIR is set. Unset means no file: under systemd stderr is
// already the journal, and on a desktop a tool writing into the checkout's
// logs/ by default was a surprise (this used to be logs/rotating.txt, relative
// to whatever the working directory happened to be, which aborted the cluster
// on a read-only rootfs). REDLINE_LOG_DIR=logs restores the old behaviour.
void setupLogging(const LoggingOptions& options);

// Pattern and level only; sinks untouched. Kept for callers that manage their
// own sinks. Prefer the options form.
void setupLogging(bool debug_enabled);

// ---- paths ------------------------------------------------------------------

namespace paths
{

// REDLINE_LOG_DIR, or nullopt when unset.
std::optional<std::string> logDir();

// Where large, deployment-specific data lives: map archives, road graphs,
// recordings. REDLINE_DATA_DIR when set (the image sets /data); otherwise the
// per-user location, $XDG_DATA_HOME/redline or ~/.local/share/redline on
// Linux and ~/Library/Application Support/redline on macOS.
std::string dataDir();

// Directory of the running executable ("" if it cannot be determined).
std::string executableDir();

// A file shipped alongside the binaries, such as an EDS under eds/. Looks for
// <executable dir>/../<relative> first -- the install layout, /opt/redline/bin
// next to /opt/redline/eds -- then walks up from the executable to find the
// checkout a developer build lives in, then REDLINE_REPO_ROOT. No build path
// is compiled in: a binary that carries its build directory fails the image's
// QA and is wrong on any other machine.
std::string resource(std::string_view relative);

// Expands a path from a config file or the command line:
//   ${NAME}            an environment variable; ${REDLINE_DATA_DIR} expands to
//                      dataDir() even when the variable is unset
//   ~/...              the home directory
//   relative paths     resolved against `relative_to` when it is given (a config
//                      file's directory), otherwise left alone
std::string expand(std::string_view value, std::string_view relative_to = {});

}  // namespace paths

// ---- systemd ----------------------------------------------------------------

namespace systemd
{

// Tells the service manager this process is ready (sd_notify "READY=1").
// True when the message was sent; false, silently, when NOTIFY_SOCKET is not
// set, which is every run outside a Type=notify unit. No libsystemd needed.
bool notifyReady();

// Also sends a free-form status line, shown by `systemctl status`.
bool notifyStatus(std::string_view status);

}  // namespace systemd

}  // namespace core

#endif  // CORE_H_
