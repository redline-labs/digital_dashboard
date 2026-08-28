#include "pub_sub/node_identity.h"
#include "scope/command_line_args.h"
#include "scope/scope_methods.h"
#include "scope/settings.h"
#include "scope/scope_window.h"

#include "agent_control/log_sink.h"
#include "agent_control/methods.h"
#include "agent_control/server.h"
#include "agent_control/zenoh_methods.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>

#include <QApplication>
#include <QTimer>

int main(int argc, char** argv)
{
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    // Parse before touching sinks or Qt: --mcp changes both where logs go and
    // which platform plugin QApplication will pick, and the platform can only
    // be chosen before QApplication is constructed.
    auto args = scope::parseCommandLineArgs(argc, argv);
    if (!args)
    {
        return -1;
    }

    const bool agent_mode = args->mcp_socket_path.has_value();

    if (agent_mode)
    {
        // Headless, always. No window manager, no display, no way for a stray
        // window to steal focus on a developer's desktop.
        qputenv("QT_QPA_PLATFORM", "offscreen");

        // stdout carries the AGENT_READY handshake line and nothing else, so
        // the supervising process can parse it without wading through log
        // output. Logs go to stderr, which the companion server captures for
        // post-mortem after a crash (the in-process ring dies with the process).
        spdlog::default_logger()->sinks().clear();
        spdlog::default_logger()->sinks().push_back(
            std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

        // The queryable ring behind app.logs, plus the bridge that routes Qt's
        // own diagnostics into the same stream.
        agent_control::installLogCapture();
    }
    else
    {
        const size_t max_size_bytes = 5u * 1024u * 1024u;
        const size_t max_files = 3u;
        spdlog::default_logger()->sinks().push_back(
            std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                "logs/scope.txt", max_size_bytes, max_files, true));
    }

    spdlog::set_level(args->debug_enabled ? spdlog::level::debug : spdlog::level::info);

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. This
    // app subscribes but never publishes, so without it the process is
    // invisible on the bus entirely. See pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("scope");

    // Set BEFORE QStandardPaths is asked anything: it builds the per-user
    // config path out of these, so settingsPath() would otherwise resolve
    // somewhere generic and the settings a user saved would not be found on the
    // next run. See scope/settings.h.
    QCoreApplication::setOrganizationName("redline");
    QCoreApplication::setApplicationName("scope");

    QApplication app(argc, argv);

    scope::ScopeWindow window;

    // Before the workspace: a map panel opens its archives as it is built, and
    // it looks them up by name in these.
    window.loadSettings(QString::fromStdString(
        args->settings_path.empty() ? scope::settingsPath() : args->settings_path));

    // From the same flag that chose the offscreen platform. A modal dialog in a
    // headless run has nobody to dismiss it, so the window has to know not to
    // raise one -- otherwise an unsaved-changes prompt on the way out is a hang
    // with no diagnostic at all.
    window.setHeadless(agent_mode);

    if (!args->workspace_path.empty())
    {
        if (!window.loadWorkspace(QString::fromStdString(args->workspace_path)))
        {
            SPDLOG_CRITICAL("Failed to load workspace '{}'.", args->workspace_path);
            return -1;
        }
    }

    // AFTER the workspace, so `--config w.yaml --bag drives/today` opens the
    // recording with the workspace's panels already there and binds them to it.
    // The other order would bind them to an empty source and then throw the
    // bindings away.
    if (!args->bag_path.empty())
    {
        if (!window.openRecording(QString::fromStdString(args->bag_path)))
        {
            SPDLOG_CRITICAL("Failed to open recording '{}'.", args->bag_path);
            return -1;
        }
    }

    // The window starts OFFLINE and opens no zenoh session on its own, so this
    // is the only thing that puts a freshly launched scope on the bus. Mutually
    // exclusive with --bag, which parseCommandLineArgs() has already refused.
    if (args->start_online && !window.goOnline())
    {
        SPDLOG_CRITICAL("Failed to go online.");
        return -1;
    }

    window.show();

    std::unique_ptr<agent_control::AgentServer> agent;
    if (agent_mode)
    {
        agent = std::make_unique<agent_control::AgentServer>("scope");

        agent_control::AppInfo app_info;
        app_info.app = "scope";
        app_info.config_path = args->workspace_path;
        agent_control::registerCoreMethods(*agent, app_info);

        // Publishing a known value and screenshotting the panel that plots it
        // is the fastest way to check a rendering change, so the zenoh verbs
        // are as load-bearing here as they are in the dashboard.
        agent_control::registerZenohMethods(*agent);

        scope::registerScopeMethods(*agent, window);

        if (!agent->start(*args->mcp_socket_path))
        {
            SPDLOG_CRITICAL("Failed to start the agent control interface on '{}'.",
                            *args->mcp_socket_path);
            return -1;
        }

        // The readiness handshake. A supervising process waits for this line
        // rather than polling for the socket file: the socket exists from the
        // moment bind() returns, which is before the window is up, so a poller
        // would connect too early and see an empty widget tree.
        std::cout << "AGENT_READY " << *args->mcp_socket_path << " " << ::getpid() << std::endl;
    }

    // Only a flag is set from the handler. Neither spdlog nor
    // QCoreApplication::quit() is async-signal-safe -- calling them here could
    // deadlock on a lock the interrupted thread already held, which is a hang
    // at exactly the moment you are trying to stop the process. A timer polls
    // the flag and does the real work on the GUI thread.
    static std::atomic<bool> interrupted{false};
    std::signal(SIGINT, [](int /*signum*/) { interrupted.store(true, std::memory_order_relaxed); });

    QTimer interrupt_poll;
    QObject::connect(&interrupt_poll, &QTimer::timeout, &app, [&]()
    {
        if (interrupted.load(std::memory_order_relaxed))
        {
            SPDLOG_WARN("SIGINT received, quitting.");
            QCoreApplication::quit();
        }
    });
    interrupt_poll.start(std::chrono::milliseconds{100});

    app.exec();  // Blocking.

    SPDLOG_WARN("Exit received, tearing down.");

    // Stop serving before the widgets it points at start being destroyed.
    if (agent)
    {
        agent->stop();
    }

    return 0;
}
