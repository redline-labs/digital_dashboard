#ifndef WEB_CONSOLE_HTTP_SERVER_H
#define WEB_CONSOLE_HTTP_SERVER_H

#include "node_config.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// The console's HTTP surface, kept behind a seam so that main.cpp and the route
// files do not each include 778 KB of header.
//
// BINDING IS SEPARATE FROM SERVING, deliberately. A port that is already taken
// must fail before the node reports itself ready, or systemd sees a healthy
// service that is not listening. bind() is called on the main thread and its
// result checked; serve() then runs the accept loop on its own.
namespace web_console
{

class UpdateRoutes;
class ServiceRoutes;

class HttpServer
{
public:
    // The update state is passed in rather than owned: its D-Bus connection
    // outlives any request, and progress keeps arriving whether or not a
    // browser is listening.
    HttpServer(const NodeConfig& config, UpdateRoutes& updates, ServiceRoutes& services);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Registers the routes and takes the socket. False means the port could not
    // be bound; the caller must not report ready.
    bool bind();

    // Runs the accept loop until stop(). Blocking, so callers give it a thread.
    void serve();

    void stop();

    // What bind() actually got, for logging: the configured port, or the one the
    // OS chose if the config asked for 0.
    int boundPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// What a handler answers with. A status, because reflash has real ones to
// report: 409 when RAUC is already installing, 507 when /data cannot hold the
// bundle. Defaulting to JSON because everything here is an API.
struct Reply
{
    int status { 200 };
    std::string body;
    std::string contentType { "application/json" };
};

// `body` as JSON with `status`. Every route answers through these two, so an
// error always has the one shape the page reads: {"error": "..."}.
Reply jsonReply(int status, const nlohmann::json& body);
Reply errorReply(int status, const std::string& message);

// A route's :name segments, by name.
using PathParams = std::unordered_map<std::string, std::string>;

// A body arriving in pieces, which is the entire reason cpp-httplib was chosen:
// a RAUC bundle is ~338 MB and must never exist in memory. The route supplies a
// factory; the server calls write() as chunks land and finish() at the end.
//
// `complete` is false when the client went away mid-upload, which is the case
// that must not leave a half-written bundle behind for RAUC to find.
class UploadSink
{
public:
    virtual ~UploadSink() = default;

    // False aborts the upload -- out of space, or a write failed.
    virtual bool write(const char* data, std::size_t length) = 0;
    virtual Reply finish(bool complete) = 0;
};

// Server-sent events, for install progress.
//
// Publishing is decoupled from the connections on purpose: progress arrives on
// GDBus's thread, and a slow or vanished browser must not be able to block it.
// Each subscriber gets its own queue; a subscriber that falls too far behind is
// dropped rather than allowed to grow without bound.
class EventStream
{
public:
    EventStream();
    ~EventStream();

    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;

    void publish(const std::string& event, const std::string& data);

    // Wakes every subscriber so their connections can end.
    void close();

    struct Impl;

private:
    friend class RouteRegistrar;
    std::unique_ptr<Impl> impl_;
};

// What a route file is handed. Declared here in full rather than forward
// declared, because a route file calls through it -- and defined out of line in
// http_server.cpp so that httplib's types never cross this header. Registering
// routes from separate translation units keeps each one's dependencies to
// itself: the system page needs system_info, the update routes need GDBus, and
// neither should drag the other into a rebuild.
class RouteRegistrar
{
public:
    // A GET that can choose its status and content type.
    void getReply(const std::string& pattern, std::function<Reply()> handler);

    // The same, for a pattern with :name segments, e.g. /api/schema/:name.
    void getWithParams(const std::string& pattern,
                       std::function<Reply(const PathParams&)> handler);

    // A POST with a small body, read whole -- commands, not uploads.
    void post(const std::string& pattern, std::function<Reply(const std::string& body)> handler);

    // A POST whose body is streamed. `contentLength` is what the client
    // declared, so the route can refuse before a byte is written -- and when it
    // does, it fills `refusal` to say why.
    //
    // That out-param exists because the first version collapsed every refusal
    // into 507: an install already running came back as "Insufficient Storage",
    // which is both the wrong status and useless to whoever is reading it. The
    // reason was in the journal and nowhere a browser could see.
    void postUpload(
        const std::string& pattern,
        std::function<std::unique_ptr<UploadSink>(std::uint64_t contentLength, Reply& refusal)> open);

    // An SSE endpoint fed by `stream`.
    void events(const std::string& pattern, EventStream& stream);

private:
    friend class RouteServer;

    struct Impl;
    explicit RouteRegistrar(Impl& impl) : impl_(impl) {}

    Impl& impl_;
};

// The httplib server with this node's settings and no routes of its own.
// HttpServer is one of these plus the node's routes; the tests register only
// the routes they exercise, without a bus or RAUC behind them.
class RouteServer
{
public:
    RouteServer();
    ~RouteServer();

    RouteServer(const RouteServer&) = delete;
    RouteServer& operator=(const RouteServer&) = delete;

    RouteRegistrar& routes();

    // Serves a directory's files at /. False if it could not be mounted.
    bool mount(const std::string& directory);

    // Port 0 asks the OS for one; boundPort() says which.
    bool bind(const std::string& address, int port);
    void serve();

    // Ends every event stream registered here BEFORE stopping the server. An
    // SSE worker sits in a 15 s wait for the next frame, and httplib joins its
    // workers on stop, so without this a SIGTERM waited out the heartbeat.
    void stop();

    int boundPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Defined in routes_system.cpp.
void registerSystemRoutes(RouteRegistrar& routes);

// Defined in routes_services.cpp.
void registerServiceRoutes(RouteRegistrar& routes, ServiceRoutes& state);

// Defined in routes_update.cpp. Takes the stream so the node can publish
// progress into it from the RAUC callback thread.
class UpdateRoutes;
void registerUpdateRoutes(RouteRegistrar& routes, UpdateRoutes& state);

} // namespace web_console

#endif // WEB_CONSOLE_HTTP_SERVER_H
