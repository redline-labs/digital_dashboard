#include "pub_sub/node_identity.h"
#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QSurfaceFormat>

#include "editor/canvas.h"
#include "editor/editor_window.h"

#include "agent_control/log_sink.h"
#include "agent_control/methods.h"
#include "agent_control/server.h"
#include "agent_control/zenoh_methods.h"
#include "dashboard/widget_methods.h"
#include "editor/editor_methods.h"
#include "editor/selection_frame.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <unistd.h>

#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace
{

struct EditorArgs
{
    std::string config_path;
    bool debug_enabled = false;
    std::optional<std::string> mcp_socket_path;
};

// Mirrors dashboard/dashboard/command_line_args.cpp rather than sharing it: that
// one hard-requires --config, which the editor deliberately does not (it opens
// empty and you load from the File menu).
std::optional<EditorArgs> parseArgs(int argc, char** argv)
{
    try
    {
        cxxopts::Options options("editor", "Dashboard layout editor.");

        options.add_options("optional")
            ("c,config", "Path to a YAML configuration file to open at startup.",
                cxxopts::value<std::string>())
            ("debug", "Enable debug logging.",
                cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("mcp", "Enable the agent control interface on a unix socket, and run headless "
                    "(forces the Qt platform to 'offscreen'). Defaults to /tmp/redline_agent_<pid>.sock.",
                cxxopts::value<std::string>()->implicit_value(""))
            ("h,help", "Print usage");

        auto result = options.parse(argc, argv);

        if (result.count("help") != 0)
        {
            std::cout << options.help({"optional"}) << std::endl;
            return std::nullopt;
        }

        // See the same check in dashboard/command_line_args.cpp: `--mcp <path>`
        // does not bind the path (implicit values need `=`), and silently
        // listening somewhere other than where the caller asked is worse than
        // refusing to start.
        if (!result.unmatched().empty())
        {
            for (const auto& leftover : result.unmatched())
            {
                SPDLOG_CRITICAL("Unrecognised argument '{}'.", leftover);
            }
            SPDLOG_CRITICAL("Note: --mcp takes its value with '=', as in "
                            "--mcp=/tmp/agent.sock. Bare --mcp uses the default path.");
            return std::nullopt;
        }

        EditorArgs args;
        if (result.count("config") != 0)
        {
            args.config_path = result["config"].as<std::string>();
        }
        args.debug_enabled = result["debug"].as<bool>();

        if (result.count("mcp") != 0)
        {
            std::string path = result["mcp"].as<std::string>();
            if (path.empty())
            {
                path = "/tmp/redline_agent_" + std::to_string(::getpid()) + ".sock";
            }
            args.mcp_socket_path = path;
        }

        return args;
    }
    catch (const std::exception& e)
    {
        SPDLOG_CRITICAL("Failed to parse command line arguments: {}", e.what());
        return std::nullopt;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    auto args = parseArgs(argc, argv);
    if (!args)
    {
        return -1;
    }

    const bool agent_mode = args->mcp_socket_path.has_value();

    // Must happen before QApplication is constructed -- the platform plugin is
    // chosen during its construction and cannot be changed afterwards.
    if (agent_mode)
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");

        // Keep stdout for the AGENT_READY handshake alone.
        spdlog::default_logger()->sinks().clear();
        spdlog::default_logger()->sinks().push_back(
            std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

        // The queryable ring behind app.logs, plus the Qt message bridge.
        agent_control::installLogCapture();
    }

    spdlog::set_level(args->debug_enabled ? spdlog::level::debug : spdlog::level::info);

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. This
    // app subscribes but never publishes, so without it the process is
    // invisible on the bus entirely. See pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("editor");

    QApplication app(argc, argv);

    EditorWindow w;

    // Under --mcp there is nobody at the screen: an unsaved-changes dialog on
    // close would be a hang, not a prompt.
    w.setHeadless(agent_mode);
    w.resize(1200, 800);
    w.show();

    if (!args->config_path.empty())
    {
        if (!w.loadConfigFrom(QString::fromStdString(args->config_path)))
        {
            SPDLOG_CRITICAL("Failed to load configuration file '{}'.", args->config_path);
            return -1;
        }
    }

    std::unique_ptr<agent_control::AgentServer> agent;
    if (agent_mode)
    {
        agent = std::make_unique<agent_control::AgentServer>("editor");

        agent_control::AppInfo app_info;
        app_info.app = "editor";
        app_info.config_path = args->config_path;
        agent_control::registerCoreMethods(*agent, app_info);

        // The editor rebuilds the child inside its SelectionFrame, which already
        // knows how to swap a widget out without disturbing the frame's
        // selection state or geometry.
        dashboard::agent::registerWidgetMethods(
            *agent,
            [&w](QWidget* target, const widget_config_t& cfg)
            {
                auto* frame = qobject_cast<SelectionFrame*>(target->parentWidget());
                if (frame == nullptr)
                {
                    return false;
                }

                // Through the canvas's history: a config applied by an agent is
                // as undoable as one applied from the properties panel.
                const Canvas::EditTransaction tx(w.canvas(), Canvas::EditSource::Widget);

                bool applied = false;
                std::visit(
                    [&](const auto& typed)
                    {
                        if constexpr (!std::is_same_v<std::decay_t<decltype(typed)>,
                                                      std::monostate>)
                        {
                            // Report what applyConfig actually did. This used to
                            // set applied unconditionally, so a config rejected
                            // for the wrong type came back to the agent as a
                            // success.
                            applied = frame->applyConfig(typed);
                        }
                    },
                    cfg.config);

                return applied;
            });

        // The editor previews live widgets, so injecting data is just as useful
        // here as in the dashboard.
        agent_control::registerZenohMethods(*agent);

        // Editor-specific verbs. These run on the GUI thread like every other
        // handler, so they can call into the window directly.
        editor::agent::registerEditorMethods(*agent, w);

        if (!agent->start(*args->mcp_socket_path))
        {
            SPDLOG_CRITICAL("Failed to start the agent control interface on '{}'.",
                            *args->mcp_socket_path);
            return -1;
        }

        std::cout << "AGENT_READY " << *args->mcp_socket_path << " " << ::getpid() << std::endl;
    }

    const int rc = app.exec();

    if (agent)
    {
        agent->stop();
    }

    return rc;
}
