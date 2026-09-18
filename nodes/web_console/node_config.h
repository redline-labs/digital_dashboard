#ifndef WEB_CONSOLE_NODE_CONFIG_H
#define WEB_CONSOLE_NODE_CONFIG_H

#include <cstdint>
#include <string>

// Where the web console listens and what it serves.
//
// Only what is wired today. A key that parses and does nothing is worse than a
// key that does not exist: the whitelist below exists precisely so a typo is an
// error rather than a setting that silently has no effect, and an unimplemented
// key defeats that. The upload and install settings arrive with the reflash
// route that needs them.
namespace web_console
{

struct NodeConfig
{
    // Loopback by default, deliberately. The shipped YAML overrides this to
    // 0.0.0.0 with a comment saying so -- the point is that a build with no
    // config, or a developer running it on a laptop, is not listening to the
    // network by accident.
    std::string bindAddress { "127.0.0.1" };

    uint16_t port { 8080 };

    // The static assets. Empty means ask core::paths::resource("web"), which
    // resolves next to the executable on the image and in the checkout
    // otherwise, so neither case needs this set.
    std::string assetDir;

    // Where uploaded bundles are staged. /data is the only writable filesystem
    // on the image, which is why the unit carries RequiresMountsFor=/data.
    std::string uploadDir { "${REDLINE_DATA_DIR}/updates" };

    // "system" or "session". The session bus exists so the whole reflash path
    // -- upload, install, progress, failure -- can be exercised against
    // tools/rauc_stub on a workstation. A board is always "system", and running
    // there on "session" would silently find no RAUC at all.
    std::string raucBus { "system" };

    // ${REDLINE_DATA_DIR} and ~ are expanded when the file is read, so the
    // values here are always usable paths.
};

// Reads the file. Returns false and logs every problem it found rather than the
// first, so a config with three typos takes one run to fix.
bool load_node_config(const std::string& path, NodeConfig& out);

// Exposed for tests.
bool parse_node_config(const std::string& yaml, NodeConfig& out);

} // namespace web_console

#endif // WEB_CONSOLE_NODE_CONFIG_H
