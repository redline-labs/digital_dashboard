// The console's routes over a real loopback socket, with no bus and no RAUC
// behind them: what a browser sees when it uploads, listens for progress, and
// when the node is told to stop.

#include "http_server.h"
#include "httplib_include.h"
#include "update_routes.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace
{

using namespace web_console;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// A scratch upload directory, removed afterwards.
struct ScratchDir
{
    ScratchDir()
    {
        std::string pattern = (fs::temp_directory_path() / "web_console_test_XXXXXX").string();
        if (::mkdtemp(pattern.data()) != nullptr) { path = pattern; }
    }
    ~ScratchDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

// The update routes on a loopback port, served on a thread of their own.
// UpdateRoutes::start() is never called, so there is no RAUC: the upload and
// stream routes do not need one.
struct Harness
{
    explicit Harness(const fs::path& uploadDir) : updates(configFor(uploadDir))
    {
        registerUpdateRoutes(server.routes(), updates);
        bound = server.bind("127.0.0.1", 0);
        if (bound)
        {
            serving = std::thread([this] { server.serve(); });
        }
    }

    ~Harness() { stop(); }

    void stop()
    {
        if (serving.joinable())
        {
            server.stop();
            serving.join();
        }
    }

    static NodeConfig configFor(const fs::path& uploadDir)
    {
        NodeConfig config;
        config.uploadDir = uploadDir.string();
        return config;
    }

    httplib::Client client() const { return httplib::Client("127.0.0.1", server.boundPort()); }

    UpdateRoutes updates;
    RouteServer server;
    bool bound { false };
    std::thread serving;
};

// SIGTERM used to wait out the progress stream's 15 s heartbeat, because the
// stream was only closed in ~UpdateRoutes, after the server had been joined.
void testStopEndsAnOpenProgressStreamPromptly()
{
    ScratchDir dir;
    Harness harness(dir.path);
    check(harness.bound, "the harness binds a loopback port");
    if (!harness.bound) { return; }

    std::atomic<bool> receiving { false };
    std::thread browser([&] {
        httplib::Client client = harness.client();
        client.Get("/api/update/events", [&](const char*, std::size_t) {
            receiving = true;
            return true;
        });
    });

    // Publish until the browser has a frame, which proves it is subscribed and
    // its worker is parked in the stream's wait.
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!receiving && Clock::now() < deadline)
    {
        harness.updates.events().publish("progress", "{}");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(receiving, "the stream delivers a published frame");

    const auto started = Clock::now();
    harness.stop();
    browser.join();
    const auto took = Clock::now() - started;
    check(took < std::chrono::seconds(3),
          "stop() ends an open progress stream promptly (took " +
              std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) +
              " ms)");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    testStopEndsAnOpenProgressStreamPromptly();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
