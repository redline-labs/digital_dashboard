#include "agent_control/methods.h"

#include "agent_control/capture.h"
#include "agent_control/inspector.h"
#include "agent_control/input.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QRect>
#include <QTimer>

#include <unistd.h>

#include <utility>

namespace agent_control
{

namespace
{

// Parameter accessors that fail loudly rather than defaulting. A silently
// ignored misspelled parameter is the kind of thing that makes an agent believe
// it did something it did not.
Result<std::string> optString(const json& params, const char* key, std::string fallback)
{
    if (!params.contains(key))
    {
        return fallback;
    }
    if (!params[key].is_string())
    {
        return std::unexpected(badParams(std::string("'") + key + "' must be a string."));
    }
    return params[key].get<std::string>();
}

Result<bool> optBool(const json& params, const char* key, bool fallback)
{
    if (!params.contains(key))
    {
        return fallback;
    }
    if (!params[key].is_boolean())
    {
        return std::unexpected(badParams(std::string("'") + key + "' must be a boolean."));
    }
    return params[key].get<bool>();
}

Result<int> optInt(const json& params, const char* key, int fallback)
{
    if (!params.contains(key))
    {
        return fallback;
    }
    if (!params[key].is_number_integer())
    {
        return std::unexpected(badParams(std::string("'") + key + "' must be an integer."));
    }
    return params[key].get<int>();
}

// Resolves the "target" parameter. Required unless `optional` is set, in which
// case a missing target yields nullptr (meaning "the focus widget" for keys, or
// "all top-level windows" for snapshots).
Result<QWidget*> resolveTarget(AgentServer& server, const json& params, bool optional)
{
    if (!params.contains("target"))
    {
        if (optional)
        {
            return nullptr;
        }
        return std::unexpected(badParams("'target' is required."));
    }
    if (!params["target"].is_string())
    {
        return std::unexpected(badParams("'target' must be a string selector."));
    }
    return server.locator().resolve(params["target"].get<std::string>());
}

// Shared by every input method: after acting, optionally return a screenshot in
// the same response. Halves the round-trips in a look-act-look loop, which is
// the dominant pattern when driving a UI.
void maybeAttachScreenshot(AgentServer& server, const json& params, QWidget* widget, json& out)
{
    const auto want = optBool(params, "screenshot", false);
    if (!want.has_value() || !want.value() || widget == nullptr)
    {
        return;
    }

    CaptureOptions capture_options;
    if (const auto max_dim = optInt(params, "max_dim", 1024); max_dim.has_value())
    {
        capture_options.max_dim = max_dim.value();
    }

    // A screenshot of the acted-on widget alone is usually too narrow to judge
    // an effect by, so capture its window.
    QWidget* shot_target = widget->window() != nullptr ? widget->window() : widget;
    auto shot = captureWidget(server.locator(), shot_target, capture_options);
    if (shot.has_value())
    {
        out["screenshot"] = std::move(shot.value());
    }
    else
    {
        out["screenshot_error"] = shot.error().toJson();
    }
}

}  // namespace

void registerCoreMethods(AgentServer& server, AppInfo info)
{
    AgentServer* server_ptr = &server;

    // ---------------------------------------------------------------- app.info
    server.registerMethod(
        "app.info",
        [server_ptr, info](const json&) -> MethodResult
        {
            json out = json::object();
            out["app"] = info.app;
            out["config_path"] = info.config_path;
            out["pid"] = static_cast<int>(::getpid());
            out["qt_version"] = qVersion();
            out["platform"] = QApplication::platformName().toStdString();
            out["revision"] = server_ptr->locator().revision();

            json windows = json::array();
            for (QWidget* w : server_ptr->locator().roots())
            {
                json entry = json::object();
                entry["path"] = WidgetLocator::pathOf(w).toStdString();
                entry["class"] = w->metaObject()->className();
                entry["title"] = w->windowTitle().toStdString();
                entry["size"] = json::array({w->width(), w->height()});
                entry["visible"] = w->isVisible();
                windows.push_back(std::move(entry));
            }
            out["windows"] = std::move(windows);
            return out;
        });

    // ---------------------------------------------------------------- app.quit
    server.registerMethod(
        "app.quit",
        [](const json&) -> MethodResult
        {
            // Deferred so this call still gets its response written before the
            // event loop stops.
            QTimer::singleShot(0, []() { QCoreApplication::quit(); });
            json out = json::object();
            out["quitting"] = true;
            return out;
        },
        AgentServer::MethodKind::kReadOnly);

    // ------------------------------------------------------------ ui.snapshot
    server.registerMethod(
        "ui.snapshot",
        [server_ptr](const json& params) -> MethodResult
        {
            auto target = resolveTarget(*server_ptr, params, /*optional=*/true);
            if (!target.has_value())
            {
                return std::unexpected(target.error());
            }

            SnapshotOptions options;
            options.root = target.value();

            const auto depth = optInt(params, "depth", -1);
            if (!depth.has_value())
            {
                return std::unexpected(depth.error());
            }
            options.max_depth = depth.value();

            const auto interactive = optBool(params, "interactive_only", false);
            if (!interactive.has_value())
            {
                return std::unexpected(interactive.error());
            }
            options.interactive_only = interactive.value();

            const auto invisible = optBool(params, "include_invisible", false);
            if (!invisible.has_value())
            {
                return std::unexpected(invisible.error());
            }
            options.include_invisible = invisible.value();

            return buildSnapshot(server_ptr->locator(), options);
        });

    // ---------------------------------------------------------------- ui.find
    server.registerMethod(
        "ui.find",
        [server_ptr](const json& params) -> MethodResult
        {
            const auto query = optString(params, "query", "");
            if (!query.has_value())
            {
                return std::unexpected(query.error());
            }
            if (query.value().empty())
            {
                return std::unexpected(badParams("'query' is required."));
            }

            const QString needle = QString::fromStdString(query.value());
            json matches = json::array();
            for (QWidget* widget : server_ptr->locator().allWidgets())
            {
                const QString class_name = QString::fromUtf8(widget->metaObject()->className());
                const bool hit = widget->objectName().contains(needle, Qt::CaseInsensitive) ||
                                 class_name.contains(needle, Qt::CaseInsensitive);
                if (hit)
                {
                    matches.push_back(describeWidget(server_ptr->locator(), widget));
                }
            }

            json out = json::object();
            out["query"] = query.value();
            out["count"] = matches.size();
            out["matches"] = std::move(matches);
            out["revision"] = server_ptr->locator().revision();
            return out;
        });

    // ----------------------------------------------------------- ui.screenshot
    server.registerMethod(
        "ui.screenshot",
        [server_ptr](const json& params) -> MethodResult
        {
            auto target = resolveTarget(*server_ptr, params, /*optional=*/true);
            if (!target.has_value())
            {
                return std::unexpected(target.error());
            }

            QWidget* widget = target.value();
            if (widget == nullptr)
            {
                const auto roots = server_ptr->locator().roots();
                if (roots.empty())
                {
                    return std::unexpected(
                        internalError("No top-level window to capture."));
                }
                widget = roots.front();
            }

            CaptureOptions options;

            const auto max_dim = optInt(params, "max_dim", 1024);
            if (!max_dim.has_value())
            {
                return std::unexpected(max_dim.error());
            }
            options.max_dim = max_dim.value();

            const auto if_changed = optString(params, "if_changed_from", "");
            if (!if_changed.has_value())
            {
                return std::unexpected(if_changed.error());
            }
            options.if_changed_from = if_changed.value();

            if (params.contains("region"))
            {
                const auto& region = params["region"];
                if (!region.is_array() || region.size() != 4u ||
                    !std::all_of(region.begin(), region.end(),
                                 [](const json& v) { return v.is_number_integer(); }))
                {
                    return std::unexpected(
                        badParams("'region' must be [x, y, width, height] integers, "
                                  "in widget-local logical pixels."));
                }
                options.region = QRect(region[0].get<int>(), region[1].get<int>(),
                                       region[2].get<int>(), region[3].get<int>());
            }

            return captureWidget(server_ptr->locator(), widget, options);
        });

    // ------------------------------------------------------------ input.click
    server.registerMethod(
        "input.click",
        [server_ptr](const json& params) -> MethodResult
        {
            auto target = resolveTarget(*server_ptr, params, /*optional=*/false);
            if (!target.has_value())
            {
                return std::unexpected(target.error());
            }

            ClickOptions options;

            const bool has_x = params.contains("x");
            const bool has_y = params.contains("y");
            if (has_x != has_y)
            {
                return std::unexpected(
                    badParams("'x' and 'y' must be given together, or both omitted "
                              "to click the widget centre."));
            }
            if (has_x)
            {
                if (!params["x"].is_number() || !params["y"].is_number())
                {
                    return std::unexpected(badParams("'x' and 'y' must be numbers."));
                }
                options.pos = QPointF(params["x"].get<double>(), params["y"].get<double>());
                options.pos_specified = true;
            }

            const auto button_name = optString(params, "button", "left");
            if (!button_name.has_value())
            {
                return std::unexpected(button_name.error());
            }
            const auto button = parseMouseButton(button_name.value());
            if (!button.has_value())
            {
                return std::unexpected(button.error());
            }
            options.button = button.value();

            const auto modifier_spec = optString(params, "modifiers", "");
            if (!modifier_spec.has_value())
            {
                return std::unexpected(modifier_spec.error());
            }
            const auto modifiers = parseModifiers(modifier_spec.value());
            if (!modifiers.has_value())
            {
                return std::unexpected(modifiers.error());
            }
            options.modifiers = modifiers.value();

            const auto count = optInt(params, "count", 1);
            if (!count.has_value())
            {
                return std::unexpected(count.error());
            }
            if (count.value() < 1 || count.value() > 2)
            {
                return std::unexpected(badParams("'count' must be 1 or 2."));
            }
            options.count = count.value();

            auto result = sendClick(target.value(), options);
            if (!result.has_value())
            {
                return std::unexpected(result.error());
            }

            json out = std::move(result.value());
            maybeAttachScreenshot(*server_ptr, params, target.value(), out);
            return out;
        },
        AgentServer::MethodKind::kMutating);

    // -------------------------------------------------------------- input.key
    server.registerMethod(
        "input.key",
        [server_ptr](const json& params) -> MethodResult
        {
            auto target = resolveTarget(*server_ptr, params, /*optional=*/true);
            if (!target.has_value())
            {
                return std::unexpected(target.error());
            }

            const auto sequence = optString(params, "keys", "");
            if (!sequence.has_value())
            {
                return std::unexpected(sequence.error());
            }
            if (sequence.value().empty())
            {
                return std::unexpected(
                    badParams("'keys' is required, e.g. 'Ctrl+S' or 'Delete'."));
            }

            auto result =
                sendKeySequence(target.value(), QString::fromStdString(sequence.value()));
            if (!result.has_value())
            {
                return std::unexpected(result.error());
            }

            json out = std::move(result.value());
            maybeAttachScreenshot(*server_ptr, params, target.value(), out);
            return out;
        },
        AgentServer::MethodKind::kMutating);

    // ------------------------------------------------------------- input.type
    server.registerMethod(
        "input.type",
        [server_ptr](const json& params) -> MethodResult
        {
            auto target = resolveTarget(*server_ptr, params, /*optional=*/true);
            if (!target.has_value())
            {
                return std::unexpected(target.error());
            }

            const auto text = optString(params, "text", "");
            if (!text.has_value())
            {
                return std::unexpected(text.error());
            }
            if (text.value().empty())
            {
                return std::unexpected(badParams("'text' is required."));
            }

            auto result = sendText(target.value(), QString::fromStdString(text.value()));
            if (!result.has_value())
            {
                return std::unexpected(result.error());
            }

            json out = std::move(result.value());
            maybeAttachScreenshot(*server_ptr, params, target.value(), out);
            return out;
        },
        AgentServer::MethodKind::kMutating);
}

}  // namespace agent_control
