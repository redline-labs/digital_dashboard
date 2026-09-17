#ifndef WEB_CONSOLE_HTTP_SERVER_H
#define WEB_CONSOLE_HTTP_SERVER_H

#include "node_config.h"

#include <functional>
#include <memory>
#include <string>

// The console's HTTP surface, kept behind a seam so that main.cpp and the route
// files do not each include 778 KB of header.
//
// BINDING IS SEPARATE FROM SERVING, deliberately. A port that is already taken
// must fail before the node reports itself ready, or systemd sees a healthy
// service that is not listening. bind() is called on the main thread and its
// result checked; serve() then runs the accept loop on its own.
namespace web_console
{

class HttpServer
{
public:
    explicit HttpServer(const NodeConfig& config);
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

// What a route file is handed. Declared here in full rather than forward
// declared, because a route file calls through it -- and defined out of line in
// http_server.cpp so that httplib's types never cross this header. Registering
// routes from separate translation units keeps each one's dependencies to
// itself: the system page needs system_info, the update routes will need GDBus,
// and neither should drag the other into a rebuild.
class RouteRegistrar
{
public:
    // Registers a GET whose body is produced on demand and served as JSON.
    void get(const std::string& pattern, std::function<std::string()> body);

private:
    friend class HttpServer;

    struct Impl;
    explicit RouteRegistrar(Impl& impl) : impl_(impl) {}

    Impl& impl_;
};

// Defined in routes_system.cpp.
void registerSystemRoutes(RouteRegistrar& routes);

} // namespace web_console

#endif // WEB_CONSOLE_HTTP_SERVER_H
