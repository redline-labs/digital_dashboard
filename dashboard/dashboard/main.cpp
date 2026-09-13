#include "core/core.h"
#include "pub_sub/node_identity.h"
#include "dashboard/app_config.h"
#include "dashboard/command_line_args.h"
#include "dashboard/display_binding.h"
#include "dashboard/main_window.h"

#include "agent_control/log_sink.h"
#include "agent_control/methods.h"
#include "agent_control/server.h"
#include "agent_control/zenoh_methods.h"
#include "dashboard/widget_methods.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#include <string>
#include <string_view>
#include <vector>

// Patches to third party:
// LibUSB core for debug messages.
// spdlog tweakme to lower the default log level.

int main(int argc, char** argv)
{
    // Parse before touching sinks or Qt: --mcp changes both where logs go and
    // which platform plugin QApplication will pick, and the platform can only be
    // chosen before QApplication is constructed.
    auto args = parse_command_line_args(argc, argv);
    if (!args)
    {
        // Parsing failed or help was shown
        return -1;
    }

    const bool agent_mode = args->mcp_socket_path.has_value();

    // Logging: pattern, level and sinks all come from libs/core, so the
    // dashboard, the tools and the nodes agree on where logs go. In agent mode
    // stdout carries the AGENT_READY handshake line and nothing else, so logs go
    // to stderr only, which the companion server captures for post-mortem.
    core::setupLogging({.program = "dashboard", .debug = args->debug_enabled, .stderr_only = agent_mode});

    if (agent_mode)
    {
        // Headless, always. No window manager, no display, no way for a stray
        // window to steal focus on a developer's desktop.
        qputenv("QT_QPA_PLATFORM", "offscreen");

        // The queryable ring behind app.logs, plus the bridge that routes Qt's
        // own diagnostics into the same stream.
        agent_control::installLogCapture();
    }


    // STARTUP TIMING.
    //
    // Time to first frame is a product requirement for an instrument cluster,
    // and the phases below have very different costs: on a LattePanda Mu the
    // QApplication constructor alone is around half a second, dwarfing config
    // parsing and widget construction put together. Without these marks the
    // only way to see that is to correlate journal timestamps against source
    // line numbers, which stops working the moment this file is edited.
    //
    // steady_clock, not system_clock: this measures an interval, and
    // system_clock can step under it when the RTC syncs mid-startup.
    const auto t_start = std::chrono::steady_clock::now();
    const auto since = [&t_start]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t_start).count();
    };

    // Load the configuration file
    SPDLOG_INFO("Loading configuration file '{}'.", args->config_file_path);
    auto cfg = load_dashboard_config(args->config_file_path);
    if (!cfg)
    {
        SPDLOG_CRITICAL("Failed to load configuration file '{}'.", args->config_file_path);
        return -1;
    }

    // Bind windows to displays only where the rootfs published some. Everywhere
    // else -- a desktop, a Mac, --mcp offscreen -- this is false and every window
    // is shown the way it always was, with the environment untouched.
    const auto env = dashboard::display::processEnvironment();
    const bool bind_displays = !agent_mode && dashboard::display::platformPublishesDisplays(env);

    if (bind_displays)
    {
        std::vector<WindowPlacement> placements;
        for (const app_config_t& window_cfg : cfg->windows)
        {
            placements.push_back({window_cfg.display, window_cfg.scale, window_cfg.width, window_cfg.height});
        }

        // Scale factors only take effect if they are in place before QApplication
        // is constructed, which is why this cannot wait for the screens to exist.
        if (const auto factors = dashboard::display::screenScaleFactors(placements, env))
        {
            qputenv("QT_SCREEN_SCALE_FACTORS", QByteArray::fromStdString(*factors));
            SPDLOG_INFO("Displays: QT_SCREEN_SCALE_FACTORS={}", *factors);
        }
    }

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. This
    // app subscribes but never publishes, so without it the process is
    // invisible on the bus entirely. See pub_sub/node_identity.h.
    SPDLOG_INFO("startup: config loaded at {} ms", since());

    pub_sub::NodeIdentity node_identity("dashboard");
    SPDLOG_INFO("startup: zenoh session at {} ms", since());

    QApplication app(argc, argv);
    SPDLOG_INFO("startup: QApplication at {} ms", since());

    // Create windows from configuration
    std::vector<std::unique_ptr<MainWindow>> windows;
    for (const app_config_t& window_cfg : cfg->windows)
    {
        const std::string_view role = reflection::enum_to_string(window_cfg.display);

        if (!bind_displays)
        {
            windows.push_back(std::make_unique<MainWindow>(window_cfg));
            windows.back()->show();
            SPDLOG_INFO("Starting window '{}' ({}x{}).", window_cfg.name, window_cfg.width, window_cfg.height);
            continue;
        }

        QScreen* screen = nullptr;
        const auto display = dashboard::display::lookupDisplay(window_cfg.display, env);
        if (display)
        {
            for (QScreen* candidate : QGuiApplication::screens())
            {
                if (dashboard::display::screenMatchesConnector(candidate->name().toStdString(), display->connector))
                {
                    screen = candidate;
                    break;
                }
            }
        }

        if (screen == nullptr)
        {
            std::string available;
            for (QScreen* candidate : QGuiApplication::screens())
            {
                available += (available.empty() ? "" : ", ") + candidate->name().toStdString();
            }
            const std::string wanted = display ? "connector '" + display->connector + "'" : "no connector published";

            // The primary falls back to wherever Qt put its primary screen, which is
            // what happened before displays were bound at all. A secondary does not:
            // on eglfs it would land full screen on top of the primary, and building
            // it would start its subscriptions -- CarPlay's included -- for nothing.
            if (window_cfg.display != display_role_t::primary)
            {
                SPDLOG_WARN("Window '{}' wants the {} display ({}); none of [{}] matches, so it is not shown.",
                            window_cfg.name, role, wanted, available);
                continue;
            }
            screen = QGuiApplication::primaryScreen();
            SPDLOG_WARN("Window '{}' wants the {} display ({}); none of [{}] matches, using '{}'.",
                        window_cfg.name, role, wanted, available,
                        screen ? screen->name().toStdString() : std::string("<none>"));
        }

        windows.push_back(std::make_unique<MainWindow>(window_cfg));
        if (screen != nullptr)
        {
            windows.back()->showOnScreen(screen);
            SPDLOG_INFO("Starting window '{}' ({}x{}) on the {} display, screen '{}' ({}x{} logical, dpr {}).",
                        window_cfg.name, window_cfg.width, window_cfg.height, role,
                        screen->name().toStdString(), screen->geometry().width(),
                        screen->geometry().height(), screen->devicePixelRatio());
        }
        else
        {
            windows.back()->show();
        }
    }

    if (windows.empty())
    {
        SPDLOG_CRITICAL("No window in '{}' has a display to go on.", args->config_file_path);
        return -1;
    }
    SPDLOG_INFO("startup: {} window(s) built and shown at {} ms", windows.size(), since());

    // Readiness means "the cluster is on screen", not "the process is running":
    // redline-mark-good and the backlight unit on the target key on it. A
    // zero-length timer runs after the expose and paint events show() posted,
    // i.e. once the first frame has been painted. No-op outside a Type=notify
    // unit (no NOTIFY_SOCKET).
    QTimer::singleShot(0, [&since]() {
        if (core::systemd::notifyReady())
        {
            SPDLOG_INFO("startup: READY sent to systemd at {} ms", since());
        }
    });

    std::unique_ptr<agent_control::AgentServer> agent;
    if (agent_mode)
    {
        agent = std::make_unique<agent_control::AgentServer>("dashboard");

        agent_control::AppInfo app_info;
        app_info.app = "dashboard";
        app_info.config_path = args->config_file_path;
        agent_control::registerCoreMethods(*agent, app_info);

        // Dashboard widgets take their config at construction, so applying a new
        // one means rebuilding the widget in place.
        dashboard::agent::registerWidgetMethods(
            *agent,
            [&windows](QWidget* target, const widget_config_t& widget_config)
            {
                // Only the window that owns the widget will take it.
                for (const auto& window : windows)
                {
                    if (window->rebuildWidget(target, widget_config))
                    {
                        return true;
                    }
                }
                return false;
            });

        // Publishing a known value and screenshotting the gauge that subscribes
        // to it is the fastest way to check a dashboard change.
        agent_control::registerZenohMethods(*agent);

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
    // deadlock on a lock the interrupted thread already held, which is a hang at
    // exactly the moment you are trying to stop the process. A timer polls the
    // flag and does the real work on the GUI thread.
    static std::atomic<bool> interrupted{false};
    std::signal(SIGINT, [](int /* signum */) { interrupted.store(true, std::memory_order_relaxed); });

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
