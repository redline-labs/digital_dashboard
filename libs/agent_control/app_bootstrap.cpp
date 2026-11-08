// SPDX-License-Identifier: GPL-3.0-or-later

#include "agent_control/app_bootstrap.h"

#include "agent_control/log_sink.h"
#include "agent_control/server.h"

#include <QtGlobal>

#include <spdlog/spdlog.h>

namespace agent_control
{

void prepareHeadless()
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    installLogCapture();
}

bool startAndAnnounce(AgentServer& server, const std::string& socket_path)
{
    if (!server.start(socket_path))
    {
        SPDLOG_CRITICAL("Failed to start the agent control interface on '{}'.", socket_path);
        return false;
    }
    announceReady(socket_path);
    return true;
}

}  // namespace agent_control
