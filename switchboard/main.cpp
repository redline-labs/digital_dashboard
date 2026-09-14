#include "switchboard/command_line_args.h"
#include "switchboard/switchboard_methods.h"
#include "switchboard/switchboard_window.h"

#include "agent_control/log_sink.h"
#include "agent_control/methods.h"
#include "agent_control/server.h"
#include "agent_control/zenoh_methods.h"
#include "core/core.h"
#include "pub_sub/node_identity.h"

#include <spdlog/spdlog.h>

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
    // Parse before touching sinks or Qt: --mcp changes both where logs go and
    // which platform plugin QApplication will pick.
    auto args = switchboard::parseCommandLineArgs(argc, argv);
    if (!args)
    {
        return -1;
    }

    const bool agent_mode = args->mcp_socket_path.has_value();

    core::setupLogging(
        {.program = "switchboard", .debug = args->debug_enabled, .stderr_only = agent_mode});

    if (agent_mode)
    {
        // Headless, always -- see scope/main.cpp.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        agent_control::installLogCapture();
    }

    QCoreApplication::setOrganizationName("redline");
    QCoreApplication::setApplicationName("switchboard");

    QApplication app(argc, argv);

    // Declared at startup, unlike scope: switchboard has no offline mode, and a
    // tool that sends requests onto the bus should be visible on it as the
    // thing sending them.
    pub_sub::NodeIdentity node_identity("switchboard");

    switchboard::SwitchboardWindow window(args->timeout_ms);
    window.show();

    std::unique_ptr<agent_control::AgentServer> agent;
    if (agent_mode)
    {
        agent = std::make_unique<agent_control::AgentServer>("switchboard");

        agent_control::AppInfo app_info;
        app_info.app = "switchboard";
        agent_control::registerCoreMethods(*agent, app_info);

        // zenoh.publish and zenoh.read, so an agent can put a service's inputs
        // on the bus and check its effects from the same socket.
        agent_control::registerZenohMethods(*agent);

        switchboard::registerSwitchboardMethods(*agent, window);

        if (!agent->start(*args->mcp_socket_path))
        {
            SPDLOG_CRITICAL("Failed to start the agent control interface on '{}'.",
                            *args->mcp_socket_path);
            return -1;
        }

        // The readiness handshake a supervisor waits for. See scope/main.cpp.
        std::cout << "AGENT_READY " << *args->mcp_socket_path << " " << ::getpid() << std::endl;
    }

    // Only a flag from the handler; the timer does the real work on the GUI
    // thread. Neither spdlog nor quit() is async-signal-safe.
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

    if (agent)
    {
        agent->stop();
    }

    return 0;
}
