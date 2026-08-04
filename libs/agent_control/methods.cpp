#include "agent_control/methods.h"

#include "agent_control/capture.h"
#include "agent_control/inspector.h"
#include "agent_control/input.h"
#include "agent_control/log_sink.h"

#include <spdlog/spdlog.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
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

    // ---------------------------------------------------------------- app.logs
    server.registerMethod(
        "app.logs",
        [](const json& params) -> MethodResult
        {
            auto ring = logRing();
            if (ring == nullptr)
            {
                return std::unexpected(AgentError{
                    ErrorCode::kUnsupportedInApp,
                    "Log capture is not installed in this process.",
                    json::object()});
            }

            RingSink::Query query;

            if (params.contains("since_seq"))
            {
                if (!params["since_seq"].is_number_unsigned())
                {
                    return std::unexpected(
                        badParams("'since_seq' must be a non-negative integer."));
                }
                query.since_seq = params["since_seq"].get<std::uint64_t>();
            }

            const auto level_name = optString(params, "level", "");
            if (!level_name.has_value())
            {
                return std::unexpected(level_name.error());
            }
            if (!level_name.value().empty())
            {
                const auto level = spdlog::level::from_str(level_name.value());
                // from_str returns `off` for anything it does not recognise, so a
                // typo would otherwise silently filter out every record.
                if (level == spdlog::level::off && level_name.value() != "off")
                {
                    return std::unexpected(badParams(
                        "Unknown level '" + level_name.value() +
                        "'; expected trace, debug, info, warn, err, critical or off."));
                }
                query.min_level = static_cast<int>(level);
            }

            const auto grep = optString(params, "grep", "");
            if (!grep.has_value())
            {
                return std::unexpected(grep.error());
            }
            query.grep = grep.value();

            const auto logger = optString(params, "logger", "");
            if (!logger.has_value())
            {
                return std::unexpected(logger.error());
            }
            query.logger = logger.value();

            const auto limit = optInt(params, "limit", 200);
            if (!limit.has_value())
            {
                return std::unexpected(limit.error());
            }
            if (limit.value() < 0)
            {
                return std::unexpected(badParams("'limit' must not be negative."));
            }
            query.limit = static_cast<std::size_t>(limit.value());

            const auto result = ring->query(query);

            json records = json::array();
            for (const LogRecord& record : result.records)
            {
                records.push_back(record.toJson());
            }

            json out = json::object();
            out["records"] = std::move(records);
            // Feed this straight back as since_seq to get only what is new.
            out["next_seq"] = result.next_seq;
            out["held"] = result.total_held;
            if (result.dropped > 0)
            {
                out["dropped"] = result.dropped;
                out["dropped_note"] =
                    "The ring wrapped: this many records were evicted before you read "
                    "them. Poll more often or raise the ring capacity.";
            }
            return out;
        });

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

            const auto annotate = optBool(params, "annotate", false);
            if (!annotate.has_value())
            {
                return std::unexpected(annotate.error());
            }
            options.annotate = annotate.value();

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

    // ------------------------------------------------------------ ui.wait_for
    server.registerMethod(
        "ui.wait_for",
        [server_ptr](const json& params) -> MethodResult
        {
            const auto condition = optString(params, "condition", "exists");
            if (!condition.has_value())
            {
                return std::unexpected(condition.error());
            }

            const auto selector = optString(params, "target", "");
            if (!selector.has_value())
            {
                return std::unexpected(selector.error());
            }
            if (selector.value().empty())
            {
                return std::unexpected(badParams("'target' is required."));
            }

            const auto timeout_ms = optInt(params, "timeout_ms", 3000);
            if (!timeout_ms.has_value())
            {
                return std::unexpected(timeout_ms.error());
            }

            const auto poll_ms = optInt(params, "poll_ms", 50);
            if (!poll_ms.has_value())
            {
                return std::unexpected(poll_ms.error());
            }

            enum class Condition
            {
                kExists,
                kVisible,
                kGone,
                kEnabled,
            };

            Condition wanted = Condition::kExists;
            if (condition.value() == "exists")      { wanted = Condition::kExists; }
            else if (condition.value() == "visible"){ wanted = Condition::kVisible; }
            else if (condition.value() == "gone")   { wanted = Condition::kGone; }
            else if (condition.value() == "enabled"){ wanted = Condition::kEnabled; }
            else
            {
                return std::unexpected(badParams(
                    "Unknown condition '" + condition.value() +
                    "'; expected exists, visible, gone or enabled."));
            }

            // This runs ON the GUI thread, so the wait must pump the event loop
            // rather than sleep -- otherwise nothing could ever change and every
            // wait would time out. That also means the caller's own dispatch
            // timeout must exceed this one, which is why the reply says so.
            QElapsedTimer timer;
            timer.start();
            bool satisfied = false;

            for (;;)
            {
                auto found = server_ptr->locator().resolve(selector.value());
                switch (wanted)
                {
                    case Condition::kExists:
                        satisfied = found.has_value();
                        break;
                    case Condition::kVisible:
                        satisfied = found.has_value() && found.value()->isVisible();
                        break;
                    case Condition::kEnabled:
                        satisfied = found.has_value() && found.value()->isEnabled();
                        break;
                    case Condition::kGone:
                        satisfied = !found.has_value() || !found.value()->isVisible();
                        break;
                }

                if (satisfied || timer.elapsed() >= timeout_ms.value())
                {
                    break;
                }
                QCoreApplication::processEvents(QEventLoop::AllEvents, poll_ms.value());
            }

            json out = json::object();
            out["target"] = selector.value();
            out["condition"] = condition.value();
            out["satisfied"] = satisfied;
            out["waited_ms"] = static_cast<int>(timer.elapsed());
            if (!satisfied)
            {
                out["note"] =
                    "Timed out. If the app is slow rather than stuck, raise timeout_ms -- "
                    "and raise _timeout_ms above it too, or the dispatcher gives up first.";
            }
            return out;
        });

    // ------------------------------------------------------------- input.drag
    server.registerMethod(
        "input.drag",
        [server_ptr](const json& params) -> MethodResult
        {
            auto target = resolveTarget(*server_ptr, params, /*optional=*/false);
            if (!target.has_value())
            {
                return std::unexpected(target.error());
            }

            DragOptions options;

            for (const char* key : {"from_x", "from_y", "to_x", "to_y"})
            {
                if (!params.contains(key) || !params[key].is_number())
                {
                    return std::unexpected(badParams(
                        "'from_x', 'from_y', 'to_x' and 'to_y' are all required numbers, "
                        "in widget-local logical pixels."));
                }
            }
            options.from = QPointF(params["from_x"].get<double>(), params["from_y"].get<double>());
            options.to = QPointF(params["to_x"].get<double>(), params["to_y"].get<double>());

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

            const auto steps = optInt(params, "steps", 10);
            if (!steps.has_value())
            {
                return std::unexpected(steps.error());
            }
            options.steps = steps.value();

            const auto hold = optInt(params, "hold_ms", 0);
            if (!hold.has_value())
            {
                return std::unexpected(hold.error());
            }
            options.hold_ms = hold.value();

            auto result = sendDrag(target.value(), options);
            if (!result.has_value())
            {
                return std::unexpected(result.error());
            }

            json out = std::move(result.value());
            maybeAttachScreenshot(*server_ptr, params, target.value(), out);
            return out;
        },
        AgentServer::MethodKind::kMutating);

    // ------------------------------------------------------------- input.drop
    server.registerMethod(
        "input.drop",
        [server_ptr](const json& params) -> MethodResult
        {
            auto target = resolveTarget(*server_ptr, params, /*optional=*/false);
            if (!target.has_value())
            {
                return std::unexpected(target.error());
            }

            if (!params.contains("x") || !params.contains("y") || !params["x"].is_number() ||
                !params["y"].is_number())
            {
                return std::unexpected(
                    badParams("'x' and 'y' are required numbers, in widget-local pixels."));
            }
            const QPointF pos(params["x"].get<double>(), params["y"].get<double>());

            if (!params.contains("mime") || !params["mime"].is_object() ||
                params["mime"].empty())
            {
                return std::unexpected(badParams(
                    "'mime' must be a non-empty object of {format: payload}, e.g. "
                    "{\"text/plain\": \"carplay\"}."));
            }

            std::vector<std::pair<QString, QByteArray>> mime;
            for (const auto& [format, payload] : params["mime"].items())
            {
                if (!payload.is_string())
                {
                    return std::unexpected(
                        badParams("mime payload for '" + format + "' must be a string."));
                }
                mime.emplace_back(QString::fromStdString(format),
                                  QByteArray::fromStdString(payload.get<std::string>()));
            }

            auto result = sendDrop(target.value(), pos, mime, Qt::CopyAction);
            if (!result.has_value())
            {
                return std::unexpected(result.error());
            }

            json out = std::move(result.value());
            maybeAttachScreenshot(*server_ptr, params, target.value(), out);
            return out;
        },
        AgentServer::MethodKind::kMutating);

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
