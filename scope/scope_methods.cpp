#include "scope/scope_methods.h"

#include "scope/data_source.h"
#include "scope/panel_registry.h"
#include "scope/scope_window.h"
#include "scope/signal_browser.h"
#include "scope/time_base.h"

#include "time_series/time_series_panel.h"

#include "agent_control/error.h"
#include "agent_control/server.h"

#include "config_codec/config_json.h"

#include <QApplication>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPoint>

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace scope
{

namespace
{

using agent_control::AgentError;
using agent_control::badParams;
using agent_control::json;
using agent_control::MethodResult;

AgentError internalError(std::string message)
{
    AgentError error;
    error.code = agent_control::ErrorCode::kInternal;
    error.message = std::move(message);
    return error;
}

// Every panel-addressed method needs this, and all of them should fail the same
// way: naming a panel that is not there is a caller error, and the reply lists
// what there is so the next call can be right.
std::expected<ScopeWindow::PanelEntry*, AgentError> panelFrom(ScopeWindow& window,
                                                              const json& params)
{
    const auto id = params.find("panel");
    if (id == params.end() || !id->is_string())
    {
        return std::unexpected(badParams("'panel' (string) is required."));
    }

    const QString panel_id = QString::fromStdString(id->get<std::string>());
    ScopeWindow::PanelEntry* entry = window.findPanel(panel_id);
    if (entry == nullptr)
    {
        AgentError error = badParams("No panel with id '" + id->get<std::string>() + "'.");
        json known = json::array();
        for (const ScopeWindow::PanelEntry& candidate : window.panels())
        {
            known.push_back(candidate.id.toStdString());
        }
        error.data["known_panels"] = known;
        return std::unexpected(std::move(error));
    }
    return entry;
}

json describePanel(const ScopeWindow::PanelEntry& entry)
{
    json out;
    out["id"] = entry.id.toStdString();
    out["type"] = std::string(
        reflection::enum_traits<panel_type_t>::to_string(entry.panel->panelType()));
    out["title"] = entry.panel->title().toStdString();
    out["floating"] = entry.dock->isFloating();
    out["visible"] = entry.dock->isVisible();

    const QRect rect = entry.panel->geometry();
    out["rect"] = json::array({rect.x(), rect.y(), rect.width(), rect.height()});

    if (const auto* plot = qobject_cast<const TimeSeriesPanel*>(entry.panel))
    {
        json traces = json::array();
        for (const signal_binding_t& binding : plot->getConfig().traces)
        {
            json trace;
            trace["zenoh_key"] = binding.zenoh_key;
            trace["schema_type"] = std::string(reflection::enum_to_string(binding.schema_type));
            trace["value_expression"] = binding.value_expression;
            trace["label"] = binding.label;
            trace["units"] = binding.units;
            trace["color"] = binding.color.value();
            traces.push_back(std::move(trace));
        }
        out["traces"] = std::move(traces);
    }

    return out;
}

json candidateToJson(const BindingCandidate& candidate)
{
    json out;
    out["zenoh_key"] = candidate.zenoh_key;
    out["schema_name"] = candidate.schema_name;
    out["field_name"] = candidate.field_name;
    out["type_category"] = candidate.type_category;
    out["numeric"] = candidate.isNumeric();
    out["topic_level"] = candidate.isTopicLevel();
    return out;
}

}  // namespace

void registerScopeMethods(agent_control::AgentServer& server, ScopeWindow& window)
{
    ScopeWindow* const win = &window;

    // ------------------------------------------------------------ composition

    server.registerMethod(
        "scope.panels",
        [win](const json& /*params*/) -> MethodResult {
            json panels = json::array();
            for (const ScopeWindow::PanelEntry& entry : win->panels())
            {
                panels.push_back(describePanel(entry));
            }

            json types = json::array();
            for (const PanelTypeInfo& info : availablePanelTypes())
            {
                types.push_back(json{{"type", std::string(info.name)},
                                     {"friendly_name", std::string(info.friendly_name)}});
            }

            return json{{"panels", std::move(panels)}, {"available_types", std::move(types)}};
        });

    server.registerMethod(
        "scope.add_panel",
        [win](const json& params) -> MethodResult {
            const auto type_name = params.find("type");
            if (type_name == params.end() || !type_name->is_string())
            {
                return std::unexpected(badParams("'type' (string) is required."));
            }

            const auto type = reflection::enum_traits<panel_type_t>::try_from_string(
                type_name->get<std::string>());
            if (!type || *type == panel_type_t::unknown)
            {
                AgentError error =
                    badParams("Unknown panel type '" + type_name->get<std::string>() + "'.");
                json known = json::array();
                for (const PanelTypeInfo& info : availablePanelTypes())
                {
                    known.push_back(std::string(info.name));
                }
                error.data["known_types"] = known;
                return std::unexpected(std::move(error));
            }

            QString id;
            if (const auto requested = params.find("id");
                requested != params.end() && requested->is_string())
            {
                id = QString::fromStdString(requested->get<std::string>());
            }

            const QString created = win->addPanel(*type, id);
            if (created.isEmpty())
            {
                return std::unexpected(internalError("Failed to create the panel."));
            }

            ScopeWindow::PanelEntry* entry = win->findPanel(created);
            json out = describePanel(*entry);
            out["added"] = true;
            return out;
        },
        agent_control::AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "scope.remove_panel",
        [win](const json& params) -> MethodResult {
            const auto entry = panelFrom(*win, params);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }
            const QString id = entry.value()->id;
            return json{{"removed", win->removePanel(id)}, {"id", id.toStdString()}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // ----------------------------------------------------------------- signals

    server.registerMethod(
        "scope.browser",
        [win](const json& /*params*/) -> MethodResult {
            json candidates = json::array();
            for (const BindingCandidate& candidate : win->browser()->candidates())
            {
                candidates.push_back(candidateToJson(candidate));
            }

            return json{
                {"candidates", std::move(candidates)},
                // Said every time, because an empty list here does NOT mean the
                // bus is empty and a caller that assumes otherwise will report
                // a dead system that is merely idle.
                {"note",
                 "Topics are listed from advertisements: a publisher declares a zenoh "
                 "liveliness token when it starts, so a topic appears here whether or not it "
                 "has ever published. An empty list means no publisher is running."}};
        });

    server.registerMethod(
        "scope.add_signal",
        [win](const json& params) -> MethodResult {
            const auto entry = panelFrom(*win, params);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }

            const auto key = params.find("zenoh_key");
            if (key == params.end() || !key->is_string())
            {
                return std::unexpected(badParams("'zenoh_key' (string) is required."));
            }

            BindingCandidate candidate;

            // Two ways in. Naming a field asks the browser what it knows about
            // it, which is how a caller avoids having to know the schema. Or
            // spell out schema and category directly, which works before any
            // scan has happened.
            const auto field = params.find("field");
            const auto schema = params.find("schema");
            if (field != params.end() && field->is_string() && schema == params.end())
            {
                if (!win->browser()->findCandidate(
                        QString::fromStdString(key->get<std::string>()),
                        QString::fromStdString(field->get<std::string>()), candidate))
                {
                    AgentError error = badParams(
                        "The browser has not seen field '" + field->get<std::string>() +
                        "' on '" + key->get<std::string>() +
                        "'. Pass 'schema' and 'type_category' explicitly, or check "
                        "scope.browser for what is advertised.");
                    return std::unexpected(std::move(error));
                }
            }
            else
            {
                if (schema == params.end() || !schema->is_string())
                {
                    return std::unexpected(
                        badParams("'schema' (string) is required when 'field' is not resolvable "
                                  "through the browser."));
                }
                candidate.zenoh_key = key->get<std::string>();
                candidate.schema_name = schema->get<std::string>();
                candidate.field_name =
                    field != params.end() && field->is_string() ? field->get<std::string>() : "";
                candidate.type_category = params.value("type_category", std::string{"float"});
            }

            Panel* const panel = entry.value()->panel;
            if (!panel->acceptsBinding(candidate))
            {
                AgentError error = badParams("This panel will not accept that candidate.");
                error.data["candidate"] = candidateToJson(candidate);
                return std::unexpected(std::move(error));
            }

            if (!panel->addBinding(candidate))
            {
                // Accepted in principle but declined in fact -- a duplicate, or
                // a schema not in the registry. Not an error the caller can fix
                // by retrying, so say which it was as plainly as possible.
                AgentError error =
                    badParams("The panel declined the candidate (already plotted, or its schema "
                              "is not in the registry).");
                error.data["candidate"] = candidateToJson(candidate);
                return std::unexpected(std::move(error));
            }

            return json{{"added", true}, {"panel", describePanel(*entry.value())}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "scope.remove_signal",
        [win](const json& params) -> MethodResult {
            const auto entry = panelFrom(*win, params);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }

            const auto index = params.find("index");
            if (index == params.end() || !index->is_number_unsigned())
            {
                return std::unexpected(badParams("'index' (unsigned) is required."));
            }

            auto* plot = qobject_cast<TimeSeriesPanel*>(entry.value()->panel);
            if (plot == nullptr)
            {
                return std::unexpected(badParams("That panel has no removable signals."));
            }

            if (!plot->removeSignal(index->get<std::size_t>()))
            {
                return std::unexpected(badParams("No signal at that index."));
            }
            return json{{"removed", true}, {"panel", describePanel(*entry.value())}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // Drives the drop TARGET directly, because QDrag::exec() runs a nested event
    // loop that reads real platform events and cannot be advanced by synthesized
    // ones -- on the offscreen platform it may not run at all. Sending the
    // DragEnter -> DragMove -> Drop triple here exercises the accept/reject
    // logic and the drop handler, which is where the behaviour worth testing
    // lives; only the few lines inside exec() are left uncovered. Same
    // arrangement, and the same reasoning, as editor.palette_drag.
    server.registerMethod(
        "scope.browser_drag",
        [win](const json& params) -> MethodResult {
            const auto entry = panelFrom(*win, params);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }

            const auto key = params.find("zenoh_key");
            const auto field = params.find("field");
            if (key == params.end() || !key->is_string() || field == params.end() ||
                !field->is_string())
            {
                return std::unexpected(
                    badParams("'zenoh_key' and 'field' (strings) are required."));
            }

            BindingCandidate candidate;
            if (!win->browser()->findCandidate(QString::fromStdString(key->get<std::string>()),
                                               QString::fromStdString(field->get<std::string>()),
                                               candidate))
            {
                return std::unexpected(badParams(
                    "The browser has not seen that field; check scope.browser for what is "
                    "advertised."));
            }

            QMimeData mime;
            mime.setData(kSignalMimeType, encodeCandidate(candidate));

            QWidget* const target = entry.value()->dock;
            const QPoint centre(target->width() / 2, target->height() / 2);

            QDragEnterEvent enter(centre, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &enter);
            const bool entered = enter.isAccepted();

            QDragMoveEvent move(centre, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &move);

            QDropEvent drop(QPointF(centre), Qt::CopyAction, &mime, Qt::LeftButton,
                            Qt::NoModifier);
            QApplication::sendEvent(target, &drop);

            return json{{"accepted", drop.isAccepted()},
                        {"drag_enter_accepted", entered},
                        {"panel", describePanel(*entry.value())}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // -------------------------------------------------------------- time base

    server.registerMethod(
        "scope.time_base",
        [win](const json& params) -> MethodResult {
            TimeBase& time_base = win->timeBase();

            if (const auto seconds = params.find("window_seconds");
                seconds != params.end() && seconds->is_number())
            {
                time_base.setWindowSeconds(seconds->get<double>());
            }

            if (const auto mode = params.find("mode"); mode != params.end() && mode->is_string())
            {
                const std::string text = mode->get<std::string>();
                if (text == "live")
                {
                    time_base.setMode(TimeBase::Mode::Live);
                }
                else if (text == "paused")
                {
                    time_base.setMode(TimeBase::Mode::Paused);
                }
                else
                {
                    return std::unexpected(
                        badParams("'mode' must be 'live' or 'paused', not '" + text + "'."));
                }
            }

            if (const auto rate = params.find("render_rate_hz");
                rate != params.end() && rate->is_number())
            {
                time_base.setRenderRateHz(rate->get<int>());
            }

            if (const auto cursor = params.find("cursor"); cursor != params.end())
            {
                if (cursor->is_null())
                {
                    time_base.setCursor(std::nullopt);
                }
                else if (cursor->is_number())
                {
                    time_base.setCursor(cursor->get<double>());
                }
                else
                {
                    return std::unexpected(
                        badParams("'cursor' must be a number or null."));
                }
            }

            const SourceCaps caps = time_base.source().caps();
            json out;
            out["window_seconds"] = time_base.windowSeconds();
            out["render_rate_hz"] = time_base.renderRateHz();
            out["mode"] = time_base.mode() == TimeBase::Mode::Live ? "live" : "paused";
            out["view_begin"] = time_base.viewBegin();
            out["view_end"] = time_base.viewEnd();
            out["now"] = time_base.source().now();
            if (time_base.cursor())
            {
                out["cursor"] = *time_base.cursor();
            }
            else
            {
                out["cursor"] = nullptr;
            }
            out["caps"] = json{{"live", caps.live}, {"seekable", caps.seekable}};
            return out;
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // ------------------------------------------------------------------ config

    server.registerMethod("scope.panel_get_config", [win](const json& params) -> MethodResult {
        const auto entry = panelFrom(*win, params);
        if (!entry)
        {
            return std::unexpected(entry.error());
        }

        auto* plot = qobject_cast<TimeSeriesPanel*>(entry.value()->panel);
        if (plot == nullptr)
        {
            return std::unexpected(badParams("That panel type has no readable config."));
        }
        return json{{"panel", entry.value()->id.toStdString()},
                    {"config", config_codec::toJson(plot->getConfig())}};
    });

    server.registerMethod("scope.panel_describe_config",
                          [win](const json& params) -> MethodResult {
                              const auto entry = panelFrom(*win, params);
                              if (!entry)
                              {
                                  return std::unexpected(entry.error());
                              }
                              return json{
                                  {"panel", entry.value()->id.toStdString()},
                                  {"schema",
                                   config_codec::describeType<TimeSeriesPanelConfig_t>()}};
                          });

    server.registerMethod(
        "scope.panel_set_config",
        [win](const json& params) -> MethodResult {
            const auto entry = panelFrom(*win, params);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }

            const auto patch = params.find("config");
            if (patch == params.end() || !patch->is_object())
            {
                return std::unexpected(badParams("'config' (object) is required."));
            }

            auto* plot = qobject_cast<TimeSeriesPanel*>(entry.value()->panel);
            if (plot == nullptr)
            {
                return std::unexpected(badParams("That panel type has no settable config."));
            }

            // Partial but all-or-nothing: only the named fields are touched, and
            // an unknown name is an error rather than a silent no-op. A caller
            // that believes it changed something it did not is the worst
            // outcome here.
            TimeSeriesPanelConfig_t updated = plot->getConfig();
            std::vector<std::string> errors;
            config_codec::applyJson(*patch, updated, "", errors);
            if (!errors.empty())
            {
                AgentError error = badParams("Config patch rejected.");
                error.data["errors"] = errors;
                return std::unexpected(std::move(error));
            }

            plot->applyConfig(updated);
            entry.value()->dock->setWindowTitle(plot->title());
            return json{{"applied", true},
                        {"config", config_codec::toJson(plot->getConfig())}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // --------------------------------------------------------------- workspace

    server.registerMethod(
        "scope.save",
        [win](const json& params) -> MethodResult {
            const auto path = params.find("path");
            if (path == params.end() || !path->is_string())
            {
                return std::unexpected(badParams("'path' (string) is required."));
            }

            const QString target = QString::fromStdString(path->get<std::string>());
            if (!win->saveWorkspace(target))
            {
                return std::unexpected(
                    internalError("Failed to write '" + path->get<std::string>() + "'."));
            }
            return json{{"saved", true}, {"path", path->get<std::string>()}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "scope.load",
        [win](const json& params) -> MethodResult {
            const auto path = params.find("path");
            if (path == params.end() || !path->is_string())
            {
                return std::unexpected(badParams("'path' (string) is required."));
            }

            if (!win->loadWorkspace(QString::fromStdString(path->get<std::string>())))
            {
                return std::unexpected(badParams("Could not load '" + path->get<std::string>() +
                                                 "'. See app.logs for the reason."));
            }

            json panels = json::array();
            for (const ScopeWindow::PanelEntry& entry : win->panels())
            {
                panels.push_back(describePanel(entry));
            }
            return json{{"loaded", true},
                        {"path", path->get<std::string>()},
                        {"panels", std::move(panels)}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // ------------------------------------------------------------------- stats

    // The verb that makes this app testable without eyeballing pixels: it is
    // what proves the buffers filled, the timestamps advanced and the values
    // are the published ones. A screenshot shows a line; this says what the
    // line is made of.
    server.registerMethod("scope.sample_stats", [win](const json& params) -> MethodResult {
        json panels = json::array();

        for (const ScopeWindow::PanelEntry& entry : win->panels())
        {
            if (params.contains("panel") &&
                entry.id.toStdString() != params.value("panel", std::string{}))
            {
                continue;
            }

            auto* plot = qobject_cast<TimeSeriesPanel*>(entry.panel);
            if (plot == nullptr)
            {
                continue;
            }

            // Not `signals`: Qt's moc defines that as a macro expanding to
            // `public:`, so even a local of that name is a syntax error.
            json signal_list = json::array();
            for (const TimeSeriesPanel::SignalStats& stats : plot->stats())
            {
                json out;
                out["label"] = stats.label;
                out["bound"] = stats.bound;
                out["retained"] = stats.retained;
                out["received"] = stats.received;
                out["dropped"] = stats.dropped;
                out["has_data"] = stats.has_data;
                if (stats.has_data)
                {
                    out["t_first"] = stats.t_first;
                    out["t_last"] = stats.t_last;
                    out["min"] = stats.min;
                    out["max"] = stats.max;
                    out["last"] = stats.last;
                }
                signal_list.push_back(std::move(out));
            }

            panels.push_back(json{{"panel", entry.id.toStdString()}, {"signals", std::move(signal_list)}});
        }

        return json{{"panels", std::move(panels)}};
    });
}

}  // namespace scope
