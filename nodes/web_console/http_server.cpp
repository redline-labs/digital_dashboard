#include "http_server.h"

#include "httplib_include.h"

#include "core/core.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>

namespace web_console
{

// The httplib half of the handle the route files are given. Keeping it here is
// what lets routes_system.cpp -- and every route file after it -- compile
// without httplib.h and its warning suppression.
struct RouteRegistrar::Impl
{
    httplib::Server& server;
};

void RouteRegistrar::get(const std::string& pattern, std::function<std::string()> body)
{
    impl_.server.Get(pattern, [body = std::move(body)](const httplib::Request&, httplib::Response& response) {
        response.set_content(body(), "application/json");
    });
}

struct HttpServer::Impl
{
    explicit Impl(const NodeConfig& c) : config(c) {}

    NodeConfig config;
    httplib::Server server;
    int boundPort { 0 };
};

HttpServer::HttpServer(const NodeConfig& config) : impl_(std::make_unique<Impl>(config)) {}

HttpServer::~HttpServer() = default;

bool HttpServer::bind()
{
    // Assets: the config's directory, or wherever this build keeps them.
    // resource() walks up from the executable rather than compiling a source
    // path in, which the image's QA rejects.
    const std::string assetDir =
        impl_->config.assetDir.empty() ? core::paths::resource("web") : impl_->config.assetDir;

    std::error_code ec;
    if (std::filesystem::is_directory(assetDir, ec))
    {
        if (!impl_->server.set_mount_point("/", assetDir))
        {
            SPDLOG_WARN("[http] '{}' could not be mounted; the API will work but no page will be served",
                        assetDir);
        }
        else
        {
            SPDLOG_INFO("[http] serving assets from {}", assetDir);
        }
    }
    else
    {
        // Not fatal: the JSON routes are the useful half on a board being
        // debugged, and a missing frontend should not stop them. Reported as an
        // absolute path where one can be formed -- core::paths::resource()
        // returns its argument unchanged when it finds nothing, so the bare
        // "web" it hands back says nothing about where it looked.
        const std::string shown = std::filesystem::absolute(assetDir, ec).string();
        SPDLOG_WARN("[http] no asset directory at '{}' (from {}); serving the API only",
                    ec ? assetDir : shown,
                    impl_->config.assetDir.empty() ? "core::paths::resource(\"web\")" : "asset_dir");
    }

    RouteRegistrar::Impl registrarImpl{impl_->server};
    RouteRegistrar routes(registrarImpl);
    registerSystemRoutes(routes);

    // int, because that is httplib's parameter type; the config keeps a
    // uint16_t so the range is validated where it is read.
    const int port = static_cast<int>(impl_->config.port);
    if (!impl_->server.bind_to_port(impl_->config.bindAddress, port))
    {
        SPDLOG_ERROR("[http] cannot bind {}:{} -- is something already listening?",
                     impl_->config.bindAddress, port);
        return false;
    }
    impl_->boundPort = port;
    return true;
}

void HttpServer::serve()
{
    impl_->server.listen_after_bind();
}

void HttpServer::stop()
{
    impl_->server.stop();
}

int HttpServer::boundPort() const
{
    return impl_->boundPort;
}

} // namespace web_console
