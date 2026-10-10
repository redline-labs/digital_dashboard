// SPDX-License-Identifier: GPL-3.0-or-later

#include "agent_control/app_bootstrap.h"

#include "agent_control/log_sink.h"
#include "agent_control/server.h"

#include <QtGlobal>

#include <spdlog/spdlog.h>

#include <iostream>
#include <unistd.h>

namespace agent_control
{

namespace
{

std::string defaultSocketPath(std::string_view app)
{
    return "/tmp/redline_" + std::string(app) + "_" + std::to_string(::getpid()) + ".sock";
}

}  // namespace

void addMcpOption(cxxopts::Options& options, std::string_view app)
{
    options.add_options("optional")(
        "mcp",
        "Enable the agent control interface on a unix socket, and run headless (forces the Qt "
        "platform to 'offscreen'). Takes its value with '=', as in --mcp=/tmp/a.sock; bare --mcp "
        "uses /tmp/redline_" + std::string(app) + "_<pid>.sock.",
        cxxopts::value<std::string>()->implicit_value(""));
}

std::optional<std::string> mcpSocketPath(const cxxopts::ParseResult& result, std::string_view app)
{
    if (result.count("mcp") == 0)
    {
        return std::nullopt;
    }
    const std::string path = result["mcp"].as<std::string>();
    return path.empty() ? defaultSocketPath(app) : path;
}

bool rejectUnmatched(const cxxopts::ParseResult& result)
{
    if (result.unmatched().empty())
    {
        return true;
    }
    for (const auto& leftover : result.unmatched())
    {
        SPDLOG_CRITICAL("Unrecognised argument '{}'.", leftover);
    }
    SPDLOG_CRITICAL("Note: --mcp takes its value with '=', as in --mcp=/tmp/agent.sock. "
                    "Bare --mcp uses the default path.");
    return false;
}

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
    std::cout << "AGENT_READY " << socket_path << " " << ::getpid() << std::endl;
    return true;
}

}  // namespace agent_control
