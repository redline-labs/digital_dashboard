#include "switchboard/command_line_args.h"
#include "switchboard/switchboard_methods.h"
#include "switchboard/switchboard_window.h"

#include "agent_control/app_bootstrap.h"
#include "agent_control/log_sink.h"
#include "agent_control/methods.h"
#include "agent_control/server.h"
#include "agent_control/zenoh_methods.h"
#include "core/core.h"
#include "pub_sub/node_identity.h"
#include "qt_helpers/quit_on_signal.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <csignal>
#include <iostream>
#include <memory>

#include <QApplication>

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
        agent_control::prepareHeadless();
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

        if (!agent_control::startAndAnnounce(*agent, *args->mcp_socket_path))
        {
            return -1;
        }
    }

    qt_helpers::quitOnSignals(&app, {SIGINT, SIGTERM});

    app.exec();  // Blocking.

    SPDLOG_WARN("Exit received, tearing down.");

    if (agent)
    {
        agent->stop();
    }

    return 0;
}
