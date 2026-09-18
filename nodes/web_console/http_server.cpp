#include "http_server.h"

#include "httplib_include.h"

#include "service_routes.h"
#include "update_routes.h"

#include "core/core.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace web_console
{
namespace
{
// 2 GiB. A slot is 2.8 GB, so nothing installable can exceed this, and it is
// large enough that the free-space check refuses first for anything realistic.
constexpr std::size_t kMaxUploadBytes = 2ULL * 1024 * 1024 * 1024;
} // namespace


// The httplib half of the handle the route files are given. Keeping it here is
// what lets routes_system.cpp -- and every route file after it -- compile
// without httplib.h and its warning suppression.
struct RouteRegistrar::Impl
{
    httplib::Server& server;
};

// ---------------------------------------------------------------------------
// EventStream
//
// Progress arrives on GDBus's thread; SSE connections are served on httplib's
// thread pool. Publishing therefore never touches a socket -- it appends to each
// subscriber's queue and wakes it. A browser that vanished, or one on a slow
// link, can then only hurt itself.
// ---------------------------------------------------------------------------

namespace
{
// Enough for a whole install's worth of progress. A subscriber that has not
// drained this many frames is not reading, and is dropped rather than allowed
// to grow without bound -- an install must not be able to exhaust memory
// because somebody closed a laptop lid.
constexpr std::size_t kMaxQueuedFrames = 256;

// Bounded so one stuck reader cannot pin a thread forever, and so the heartbeat
// below actually fires.
constexpr int kWaitMs = 15000;
}  // namespace

struct Subscriber
{
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> frames;
    bool dropped { false };
    bool closed { false };
};

struct EventStream::Impl
{
    std::mutex mutex;
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    bool closed { false };
};

EventStream::EventStream() : impl_(std::make_unique<Impl>()) {}

EventStream::~EventStream()
{
    close();
}

void EventStream::publish(const std::string& event, const std::string& data)
{
    // Pre-formatted once, not per subscriber.
    std::string frame = "event: " + event + "\ndata: " + data + "\n\n";

    std::vector<std::shared_ptr<Subscriber>> targets;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        targets = impl_->subscribers;
    }

    for (const auto& subscriber : targets)
    {
        std::lock_guard<std::mutex> guard(subscriber->mutex);
        if (subscriber->frames.size() >= kMaxQueuedFrames)
        {
            subscriber->dropped = true;
        }
        else
        {
            subscriber->frames.push_back(frame);
        }
        subscriber->wake.notify_one();
    }
}

void EventStream::close()
{
    std::vector<std::shared_ptr<Subscriber>> targets;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->closed = true;
        targets.swap(impl_->subscribers);
    }
    for (const auto& subscriber : targets)
    {
        std::lock_guard<std::mutex> guard(subscriber->mutex);
        subscriber->closed = true;
        subscriber->wake.notify_one();
    }
}

// ---------------------------------------------------------------------------
// RouteRegistrar
// ---------------------------------------------------------------------------

void RouteRegistrar::get(const std::string& pattern, std::function<std::string()> body)
{
    impl_.server.Get(pattern, [body = std::move(body)](const httplib::Request&, httplib::Response& response) {
        response.set_content(body(), "application/json");
    });
}

void RouteRegistrar::getReply(const std::string& pattern, std::function<Reply()> handler)
{
    impl_.server.Get(pattern, [handler = std::move(handler)](const httplib::Request&,
                                                             httplib::Response& response) {
        const Reply reply = handler();
        response.status = reply.status;
        response.set_content(reply.body, reply.contentType);
    });
}

void RouteRegistrar::post(const std::string& pattern,
                          std::function<Reply(const std::string&)> handler)
{
    impl_.server.Post(pattern, [handler = std::move(handler)](const httplib::Request& request,
                                                              httplib::Response& response) {
        const Reply reply = handler(request.body);
        response.status = reply.status;
        response.set_content(reply.body, reply.contentType);
    });
}

void RouteRegistrar::postUpload(
    const std::string& pattern,
    std::function<std::unique_ptr<UploadSink>(std::uint64_t, Reply&)> open)
{
    // The ContentReader overload: httplib hands us chunks as they arrive and
    // never materialises the body. This is the whole reason for the library
    // choice, so it is worth being explicit that response.body is never touched
    // on this path.
    impl_.server.Post(
        pattern,
        [open = std::move(open)](const httplib::Request& request, httplib::Response& response,
                                 const httplib::ContentReader& reader) {
            // Only used if the route refuses without saying why, which would
            // be a bug in the route rather than a state worth reporting well.
            Reply refusal{.status = 500,
                          .body = R"({"error":"upload refused"})",
                          .contentType = "application/json"};

            std::unique_ptr<UploadSink> sink =
                open(request.get_header_value_u64("Content-Length"), refusal);
            if (!sink)
            {
                response.status = refusal.status;
                response.set_content(refusal.body, refusal.contentType);
                return;
            }

            bool ok = true;
            reader([&](const char* data, std::size_t length) {
                if (!ok) { return false; }
                ok = sink->write(data, length);
                return ok;
            });

            const Reply reply = sink->finish(ok);
            response.status = reply.status;
            response.set_content(reply.body, reply.contentType);
        });
}

void RouteRegistrar::events(const std::string& pattern, EventStream& stream)
{
    EventStream::Impl* streamImpl = stream.impl_.get();

    impl_.server.Get(pattern, [streamImpl](const httplib::Request&, httplib::Response& response) {
        auto subscriber = std::make_shared<Subscriber>();
        {
            std::lock_guard<std::mutex> guard(streamImpl->mutex);
            if (streamImpl->closed)
            {
                response.status = 503;
                response.set_content(R"({"error":"shutting down"})", "application/json");
                return;
            }
            streamImpl->subscribers.push_back(subscriber);
        }

        // Proxies and browsers both behave better when told not to buffer.
        response.set_header("Cache-Control", "no-store");
        response.set_header("X-Accel-Buffering", "no");

        response.set_chunked_content_provider(
            "text/event-stream",
            [streamImpl, subscriber](std::size_t, httplib::DataSink& sink) {
                std::string out;
                {
                    std::unique_lock<std::mutex> lock(subscriber->mutex);
                    subscriber->wake.wait_for(lock, std::chrono::milliseconds(kWaitMs), [&] {
                        return !subscriber->frames.empty() || subscriber->closed ||
                               subscriber->dropped;
                    });

                    if (subscriber->dropped)
                    {
                        // Say so rather than simply going quiet: a UI that stops
                        // receiving progress should know it fell behind.
                        out = "event: error\ndata: " R"({"error":"fell behind"})" "\n\n";
                    }
                    else
                    {
                        while (!subscriber->frames.empty())
                        {
                            out += subscriber->frames.front();
                            subscriber->frames.pop_front();
                        }
                    }

                    if (subscriber->closed && out.empty())
                    {
                        sink.done();
                        return false;
                    }
                }

                // A comment frame keeps the connection alive through anything
                // that times idle sockets out, and costs one line every 15s.
                if (out.empty()) { out = ":\n\n"; }

                if (!sink.write(out.data(), out.size()))
                {
                    return false;  // the browser went away
                }
                return !subscriber->dropped;
            },
            [streamImpl, subscriber](bool) {
                std::lock_guard<std::mutex> guard(streamImpl->mutex);
                std::erase(streamImpl->subscribers, subscriber);
            });
    });
}

struct HttpServer::Impl
{
    Impl(const NodeConfig& c, UpdateRoutes& u, ::node_health::HealthMonitor& h, ServiceRoutes& s)
        : config(c), updates(u), health(h), services(s)
    {
    }

    NodeConfig config;
    UpdateRoutes& updates;
    ::node_health::HealthMonitor& health;
    ServiceRoutes& services;
    httplib::Server server;
    int boundPort { 0 };
};

HttpServer::HttpServer(const NodeConfig& config, UpdateRoutes& updates,
                       ::node_health::HealthMonitor& health, ServiceRoutes& services)
    : impl_(std::make_unique<Impl>(config, updates, health, services))
{
}

HttpServer::~HttpServer() = default;

bool HttpServer::bind()
{
    // Assets: the config's directory, or wherever this build keeps them.
    // resource() walks up from the executable rather than compiling a source
    // path in, which the image's QA rejects.
    const std::string assetDir =
        impl_->config.assetDir.empty() ? core::paths::resource("web") : impl_->config.assetDir;

    // THE WHOLE REASON THIS LIBRARY WAS CHOSEN was streaming a ~338 MB RAUC
    // bundle to disk instead of buffering it -- and cpp-httplib's default
    // CPPHTTPLIB_PAYLOAD_MAX_LENGTH is 100 MB, so every real bundle was rejected
    // with 413 after ~100 MB. Measured against the real image: a 335 MB bundle
    // stopped at 104,843,888 bytes.
    //
    // Generous rather than unlimited: the free-space pre-flight in
    // routes_update.cpp is the check that matters (it knows how big the
    // partition is), and this only needs to stop being the binding constraint.
    impl_->server.set_payload_max_length(kMaxUploadBytes);

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
    registerUpdateRoutes(routes, impl_->updates);
    registerHealthRoutes(routes, impl_->health);
    registerServiceRoutes(routes, impl_->services);

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
