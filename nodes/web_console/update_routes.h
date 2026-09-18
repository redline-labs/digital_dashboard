#ifndef WEB_CONSOLE_UPDATE_ROUTES_H
#define WEB_CONSOLE_UPDATE_ROUTES_H

#include "http_server.h"
#include "node_config.h"

#include "rauc_client/rauc_client.h"

#include <filesystem>
#include <memory>
#include <string>

// State the reflash routes need: the RAUC connection and the event stream its
// progress is published into.
//
// Owned by main() rather than by the HTTP server, because its lifetime is the
// node's: the D-Bus connection outlives any request, and progress keeps arriving
// whether or not a browser is listening.
namespace web_console
{

class UpdateRoutes
{
public:
    explicit UpdateRoutes(const NodeConfig& config);
    ~UpdateRoutes();

    UpdateRoutes(const UpdateRoutes&) = delete;
    UpdateRoutes& operator=(const UpdateRoutes&) = delete;

    // Connects to RAUC and wires its progress and completion into the stream.
    // False means RAUC could not be reached; the console still serves, and says
    // so, because the rest of it is useful on a board whose updater is broken.
    bool start(std::string& error);

    bool available() const;

    EventStream& events() { return events_; }
    rauc_client::Installer* installer() { return installer_.get(); }

    // /data/updates. The only writable place on the image, and the reason the
    // unit carries RequiresMountsFor=/data.
    std::filesystem::path uploadDir() const;

    // Where a completed upload lands, and what install operates on.
    std::filesystem::path stagedBundle() const;

private:
    NodeConfig config_;
    EventStream events_;
    std::unique_ptr<rauc_client::Installer> installer_;
};

}  // namespace web_console

#endif  // WEB_CONSOLE_UPDATE_ROUTES_H
