#ifndef WEB_CONSOLE_UPDATE_ROUTES_H
#define WEB_CONSOLE_UPDATE_ROUTES_H

#include "http_server.h"
#include "node_config.h"

#include "rauc_client/rauc_client.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// State the reflash routes need: the RAUC connection and the event stream its
// progress is published into.
//
// Owned by main() rather than by the HTTP server, because its lifetime is the
// node's: the D-Bus connection outlives any request, and progress keeps arriving
// whether or not a browser is listening.
namespace web_console
{

// The right to be the one upload in progress. Two uploads at once would each
// rename over bundle.raucb and the later one would silently win, so a second
// is refused with 409 while this is held. Released on destruction.
class UploadLease
{
public:
    UploadLease() = default;
    explicit UploadLease(std::atomic<bool>& busy) : busy_(&busy) {}
    ~UploadLease();

    UploadLease(UploadLease&& other) noexcept : busy_(std::exchange(other.busy_, nullptr)) {}
    UploadLease& operator=(UploadLease&&) = delete;
    UploadLease(const UploadLease&) = delete;
    UploadLease& operator=(const UploadLease&) = delete;

    explicit operator bool() const { return busy_ != nullptr; }

private:
    std::atomic<bool>* busy_ { nullptr };
};

class UpdateRoutes
{
public:
    explicit UpdateRoutes(const NodeConfig& config);
    ~UpdateRoutes();

    UpdateRoutes(const UpdateRoutes&) = delete;
    UpdateRoutes& operator=(const UpdateRoutes&) = delete;

    // Connects to RAUC and wires its progress and completion into the stream.
    // False means the bus could not be reached; the console still serves, and
    // says so, because the rest of it is useful on a board whose updater is
    // broken. installer() retries later.
    bool start(std::string& error);

    // Whether there is a proxy to call through. NOT whether RAUC is running:
    // it is bus-activated, so the call is what starts it, and the call is what
    // reports it missing.
    bool available();

    EventStream& events() { return events_; }

    // The connection, or null if the bus cannot be reached. A failed
    // connection is retried here, at most every few seconds, so a bus that
    // came up after the node did is found without a restart. Once connected
    // the pointer is stable for this object's life.
    rauc_client::Installer* installer();

    // Why the last connection attempt failed, for a 503 to say so.
    std::string connectError();

    // How the last conversation with RAUC went, for the node's "rauc" health
    // check. Called on every status read; `onRaucHealth` hears only changes.
    void reportRauc(bool ok, const std::string& detail);
    void onRaucHealth(std::function<void(bool ok, const std::string& detail)> callback);

    // /data/updates. The only writable place on the image, and the reason the
    // unit carries RequiresMountsFor=/data.
    std::filesystem::path uploadDir() const;

    // Where a completed upload lands, and what install operates on.
    std::filesystem::path stagedBundle() const;

    // An empty lease when another upload holds it.
    UploadLease tryBeginUpload();

private:
    bool connectLocked(std::string& error);

    NodeConfig config_;
    EventStream events_;

    std::mutex mutex_;
    std::unique_ptr<rauc_client::Installer> installer_;
    // Retrying is only for a node that start()ed: the tests build the routes
    // with no RAUC and must not go looking for one on the system bus.
    bool started_ { false };
    std::chrono::steady_clock::time_point lastAttempt_;
    std::string connectError_;
    std::function<void(bool, const std::string&)> raucHealth_;
    std::optional<std::pair<bool, std::string>> lastReport_;

    std::atomic<bool> uploading_ { false };
};

// Streams an upload straight to disk, through an fd the caller opened.
//
// Written to a dot-prefixed temporary and renamed only on success, so RAUC can
// never be handed a half-written bundle: the rename is atomic within the
// filesystem, and a crashed or abandoned upload leaves a .incoming-* file that
// the node sweeps at startup rather than something that looks installable.
class BundleSink : public UploadSink
{
public:
    BundleSink(int fd, std::filesystem::path temporary, std::filesystem::path destination,
               std::uint64_t expected, UploadLease lease);
    ~BundleSink() override;

    BundleSink(const BundleSink&) = delete;
    BundleSink& operator=(const BundleSink&) = delete;

    bool write(const char* data, std::size_t length) override;
    Reply finish(bool complete) override;

private:
    int fd_;
    std::filesystem::path temporary_;
    std::filesystem::path destination_;
    // What Content-Length promised. Zero means the client did not say (a chunked
    // upload), in which case there is nothing to check against.
    std::uint64_t expected_;
    UploadLease lease_;
    std::uint64_t received_ { 0 };
    // The errno of the write that failed, so finish() can tell a full disk
    // from a client that went away.
    int writeError_ { 0 };
    bool renamed_ { false };
};

// One /boot/loader/entries file name as the status route reports it, or
// nothing if it is not a boot entry. See bootTries() in routes_update.cpp.
std::optional<nlohmann::json> bootEntryJson(const std::string& filename);

}  // namespace web_console

#endif  // WEB_CONSOLE_UPDATE_ROUTES_H
