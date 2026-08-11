#include "scope/scope_methods.h"

#include "scope/data_source.h"
#include "scope/panel_registry.h"
#include "scope/recorded_source.h"
#include "scope/scope_recorder.h"
#include "scope/scope_window.h"
#include "scope/signal_browser.h"
#include "scope/time_base.h"

#include "time_series/time_series_panel.h"

#include "agent_control/error.h"
#include "agent_control/server.h"

#include "config_codec/config_json.h"

#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_registry.h"

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

// ------------------------------------------- serving a variant without naming it
//
// These three are why adding a panel type touches nothing in this file. Each
// visits whichever alternative is live and hands it to the reflected codec, so
// the JSON, the self-description and the type name all follow from the panel's
// own struct. monostate cannot occur for a constructed panel -- the variants are
// generated from the same table the panel classes are -- but it is answered
// rather than asserted, because an RPC that aborts the process is worse than one
// that says it has nothing.

json variantToJson(const auto& variant)
{
    json out = json::object();
    std::visit(
        [&out](const auto& value) {
            using value_t = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<value_t, std::monostate>)
            {
                out = config_codec::toJson(value);
            }
        },
        variant);
    return out;
}

json describeVariant(const auto& variant)
{
    json out = json::object();
    std::visit(
        [&out](const auto& value) {
            using value_t = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<value_t, std::monostate>)
            {
                out = config_codec::describeType<value_t>();
            }
        },
        variant);
    return out;
}

std::string panelTypeName(const Panel& panel)
{
    return std::string(reflection::enum_traits<panel_type_t>::to_string(panel.panelType()));
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

    // What this panel holds, whatever kind it is, in the order
    // `scope.remove_signal` indexes them. Type-agnostic on purpose: a caller
    // that wants to drop a binding needs the index, and reading it out of a
    // per-type key would mean learning the type list this file exists not to
    // have.
    json bindings = json::array();
    for (const QString& label : entry.panel->bindingLabels())
    {
        bindings.push_back(label.toStdString());
    }
    out["bindings"] = std::move(bindings);

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

    // The element type and index, so the 32 rows a fixed-length list expands
    // into are DISTINGUISHABLE. Without them every element of MotecPdm's
    // `values` serialises identically and a caller can see 32 rows but not say
    // which is which -- the rows exist and are useless.
    if (!candidate.element_category.empty())
    {
        out["element_category"] = candidate.element_category;
    }
    if (candidate.element_index >= 0)
    {
        out["element_index"] = candidate.element_index;
    }
    out["expression"] = candidate.defaultExpression();
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
                candidate.element_category = params.value("element_category", std::string{});

                // Whether the list declares a length is a fact about the SCHEMA,
                // so it is read from the schema rather than taken from the
                // caller. A caller that could assert it would be able to talk a
                // panel into accepting a binding the evaluator then refuses.
                if (const auto found = pub_sub::get_schema(candidate.schema_name))
                {
                    for (auto schema_field : found->asStruct().getFields())
                    {
                        if (candidate.field_name == schema_field.getProto().getName().cStr())
                        {
                            candidate.has_fixed_length =
                                pub_sub::fixedListLength(schema_field).has_value();
                        }
                    }
                }
            }

            // WHICH ELEMENT, for a list. Honoured whether the candidate came
            // from the browser or was constructed above, because a caller
            // naming `values` alone would otherwise always get element 0 -- and
            // the 32 rows the browser offers would be visible through
            // `scope.browser` and unreachable by this method.
            //
            // Naming an element of something that is not a list is a caller
            // error rather than something to ignore: the expression it would
            // produce is not the one asked for.
            if (const auto element = params.find("element_index"); element != params.end())
            {
                if (!element->is_number_integer() || element->get<int>() < 0)
                {
                    return std::unexpected(
                        badParams("'element_index', when given, must be a non-negative integer."));
                }
                if (!candidate.needsElementIndex())
                {
                    return std::unexpected(badParams(
                        "'element_index' was given but '" + candidate.field_name +
                        "' is not a list."));
                }
                candidate.element_index = element->get<int>();
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

            // Through Panel's own interface. It used to cast to
            // TimeSeriesPanel, so this answered "that panel has no removable
            // signals" for a video panel holding a stream -- a definite no about
            // a binding that was definitely there.
            if (!entry.value()->panel->removeBinding(index->get<std::size_t>()))
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

            // `mode` is validated here but APPLIED after the view movers, beside
            // `following` -- it is the same flag, and a pan clears it. Applying
            // it here would make {"mode":"live","pan":-10} depend on the order
            // the handlers happen to be written in.
            std::optional<bool> want_following;
            if (const auto mode = params.find("mode"); mode != params.end() && mode->is_string())
            {
                const std::string text = mode->get<std::string>();
                if (text == "live")
                {
                    want_following = true;
                }
                else if (text == "paused")
                {
                    want_following = false;
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

            // Playback. All three are no-ops on a live source, which has
            // nothing to seek to -- so a caller that did not read caps() first
            // gets an unchanged reply rather than an error, and the reply says
            // why.
            if (const auto rate = params.find("rate"); rate != params.end() && rate->is_number())
            {
                time_base.setRate(rate->get<double>());
            }

            // BEFORE the seek, so {"playing": true, "seek": 0} starts from the
            // sought position rather than from wherever the head already was.
            if (const auto playing = params.find("playing");
                playing != params.end() && playing->is_boolean())
            {
                time_base.setPlaying(playing->get<bool>());
            }

            // ---------------------------------------------------- the view
            //
            // AT MOST ONE of these, and the check is not pedantry. They all move
            // the window, so composing two silently produces a result nobody can
            // explain from the request -- and the caller is usually a model that
            // will then reason from the wrong position.
            {
                int movers = 0;
                for (const char* name : {"seek", "view", "pan", "zoom", "fit"})
                {
                    if (params.contains(name))
                    {
                        ++movers;
                    }
                }
                if (movers > 1)
                {
                    return std::unexpected(badParams(
                        "seek, view, pan, zoom and fit all move the view; name one."));
                }
            }

            if (const auto seek = params.find("seek"); seek != params.end() && seek->is_number())
            {
                if (!time_base.source().caps().seekable)
                {
                    return std::unexpected(badParams(
                        "This source is not seekable. Open a recording with "
                        "scope.open_recording first."));
                }
                time_base.seek(seek->get<double>());
            }

            if (const auto view = params.find("view"); view != params.end())
            {
                if (!view->is_array() || view->size() != 2 || !(*view)[0].is_number() ||
                    !(*view)[1].is_number())
                {
                    return std::unexpected(
                        badParams("'view' must be [begin, end], both numbers."));
                }
                time_base.setView((*view)[0].get<double>(), (*view)[1].get<double>());
            }

            if (const auto pan = params.find("pan"); pan != params.end() && pan->is_number())
            {
                time_base.panBy(pan->get<double>());
            }

            if (const auto zoom = params.find("zoom"); zoom != params.end())
            {
                // A bare number is the factor; an object carries an anchor. The
                // anchor is what a wheel gesture has and a keyboard shortcut does
                // not, so both shapes are worth accepting.
                double factor = 0.0;
                double anchor = (time_base.viewBegin() + time_base.viewEnd()) / 2.0;

                if (zoom->is_number())
                {
                    factor = zoom->get<double>();
                }
                else if (zoom->is_object() && zoom->contains("factor") &&
                         (*zoom)["factor"].is_number())
                {
                    factor = (*zoom)["factor"].get<double>();
                    if (zoom->contains("anchor") && (*zoom)["anchor"].is_number())
                    {
                        anchor = (*zoom)["anchor"].get<double>();
                    }
                }
                else
                {
                    return std::unexpected(badParams(
                        "'zoom' must be a factor, or {factor, anchor}. Below 1 zooms in."));
                }

                if (!(factor > 0.0))
                {
                    return std::unexpected(
                        badParams("'zoom' factor must be greater than zero."));
                }
                time_base.zoomAt(anchor, factor);
            }

            if (const auto fit = params.find("fit");
                fit != params.end() && fit->is_boolean() && fit->get<bool>())
            {
                time_base.fitAll();
            }

            // AFTER the movers, so {"pan": -10, "following": true} resolves to
            // the explicit flag rather than to the pan's side effect.
            if (const auto following = params.find("following");
                following != params.end() && following->is_boolean())
            {
                want_following = following->get<bool>();
            }
            if (want_following)
            {
                time_base.setFollowing(*want_following);
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
            out["playing"] = time_base.playing();
            out["rate"] = time_base.rate();

            // `mode` stays forever -- it is what every existing caller sends and
            // reads. `following` is the same flag under its real name.
            out["following"] = time_base.following();

            // What the view will be CLAMPED to, so a caller can see the bound it
            // is working inside rather than discovering it by being clamped. On a
            // live source this is narrower than caps: a bus has no beginning, and
            // what bounds the view is how far back the buffers still reach.
            const auto [available_begin, available_end] = time_base.availableRange();
            out["available_begin"] = available_begin;
            out["available_end"] = available_end;
            out["history_seconds"] = time_base.retentionSeconds();

            // t_begin/t_end are only meaningful when seekable, and are reported
            // regardless so a caller can see the extent it is allowed to seek
            // within rather than discovering it by being clamped.
            out["caps"] = json{{"live", caps.live},
                               {"seekable", caps.seekable},
                               {"t_begin", caps.t_begin},
                               {"t_end", caps.t_end}};
            return out;
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // ----------------------------------------------------------------- density

    // What the overview strip draws behind everything else, as numbers.
    //
    // This exists because a screenshot cannot say whether the strip's background
    // is the real shape of the recording or a plausible-looking artefact -- the
    // same reason sample_stats is the thing to reach for before a picture. The
    // bucket sum against scope.capture's `messages` is the assertion worth
    // making.
    server.registerMethod("scope.density", [win](const json& params) -> MethodResult {
        std::size_t buckets = 200;
        if (const auto requested = params.find("buckets");
            requested != params.end() && requested->is_number_unsigned())
        {
            buckets = std::min<std::size_t>(requested->get<std::size_t>(), 4096);
        }
        if (buckets == 0)
        {
            return std::unexpected(badParams("'buckets' must be at least 1."));
        }

        TimeBase& time_base = win->timeBase();
        const auto [begin, end] = time_base.availableRange();

        // Through the window, NOT straight to the source: a live source cannot
        // answer and the recorder can, and this method exists to check what the
        // overview strip is drawing. Asking the source directly would report
        // "nothing" under a strip visibly full of data.
        std::vector<std::uint32_t> counts;
        const bool exact = win->densityFor(begin, end, buckets, counts);

        json out;
        out["t_begin"] = begin;
        out["t_end"] = end;
        out["buckets"] = counts;

        // False means the source declined to answer cheaply, not that there is
        // nothing there. A bag answers from its part index, which knows HOW MANY
        // but not WHERE inside a part -- so a single-part recording is one flat
        // block and says so rather than implying detail it does not have.
        out["exact"] = exact;
        return out;
    });

    // ------------------------------------------------------------------ source

    server.registerMethod("scope.source", [win](const json& /*params*/) -> MethodResult {
        const SourceCaps caps = win->source().caps();

        json out;

        // The MODE is the thing a caller usually wants, and it is one bit:
        // online exactly when the source tails the bus. `kind` says which of
        // the two OFFLINE sources it is, because "recorded" and "nothing
        // loaded" are answered identically by caps() -- both are non-live and
        // the empty one is not seekable either -- and a caller that could not
        // tell them apart would read an empty window as a bag full of silence.
        out["mode"] = win->isOnline() ? "online" : "offline";
        out["kind"] = caps.live ? "live" : (caps.seekable ? "recorded" : "empty");
        out["caps"] = json{{"live", caps.live},
                           {"seekable", caps.seekable},
                           {"t_begin", caps.t_begin},
                           {"t_end", caps.t_end}};
        out["now"] = win->source().now();

        // A recording is decoded once per signal, on a background thread, when
        // the signal is bound. Until this is zero a trace may legitimately be
        // empty -- so a caller that screenshots or reads sample_stats before
        // then is looking at an unfinished picture, not a broken one.
        if (const auto* recorded = dynamic_cast<const RecordedSource*>(&win->source()))
        {
            out["decodes_pending"] = recorded->decodesPending();
            out["wall_clock_ns"] = recorded->wallClockNanosAt(win->source().now());
        }

        json topics = json::array();
        for (const TopicInfo& topic : win->source().topics())
        {
            topics.push_back(json{{"key", topic.key},
                                  {"schema", topic.schema},
                                  {"reachable", topic.reachable}});
        }
        out["topics"] = std::move(topics);
        return out;
    });

    server.registerMethod(
        "scope.open_recording",
        [win](const json& params) -> MethodResult {
            const auto path = params.find("path");
            if (path == params.end() || !path->is_string())
            {
                return std::unexpected(
                    badParams("'path' (string) is required -- a bag DIRECTORY, not an .mcap "
                              "file."));
            }

            if (!win->openRecording(QString::fromStdString(path->get<std::string>())))
            {
                return std::unexpected(badParams(
                    "Could not open '" + path->get<std::string>() +
                    "' as a recording. See app.logs; `bag reindex` can rebuild a missing "
                    "index."));
            }

            const SourceCaps caps = win->source().caps();
            return json{{"opened", true},
                        {"path", path->get<std::string>()},
                        {"caps", json{{"live", caps.live},
                                      {"seekable", caps.seekable},
                                      {"t_begin", caps.t_begin},
                                      {"t_end", caps.t_end}}}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // Replaced `scope.go_live`, and the old name is GONE rather than aliased.
    //
    // An alias would have kept working while meaning something subtly different:
    // go_live used to be "stop reviewing", with a capture that ran regardless,
    // and it now has to be "attach to the bus and start capturing". A caller
    // that still said go_live would get the new behaviour under the old name --
    // which is the failure that looks like a broken app rather than a renamed
    // one, and is exactly what the transport_scrubber rename avoided.
    server.registerMethod(
        "scope.set_mode",
        [win](const json& params) -> MethodResult {
            const auto mode = params.find("mode");
            if (mode == params.end() || !mode->is_string())
            {
                return std::unexpected(
                    badParams("'mode' (string) is required: 'online' or 'offline'."));
            }

            const std::string text = mode->get<std::string>();
            if (text == "online")
            {
                if (!win->goOnline())
                {
                    return std::unexpected(internalError(
                        "Refused to go online: the previous capture is unsaved and the "
                        "prompt was declined. Save it with scope.save_recording first."));
                }
            }
            else if (text == "offline")
            {
                win->goOffline();
            }
            else
            {
                return std::unexpected(
                    badParams("'mode' must be 'online' or 'offline', not '" + text + "'."));
            }

            const SourceCaps caps = win->source().caps();
            return json{{"mode", win->isOnline() ? "online" : "offline"},
                        {"caps", json{{"live", caps.live},
                                      {"seekable", caps.seekable},
                                      {"t_begin", caps.t_begin},
                                      {"t_end", caps.t_end}}}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // ----------------------------------------------------------------- capture

    server.registerMethod("scope.capture", [win](const json& /*params*/) -> MethodResult {
        ScopeRecorder* const recorder = win->recorder();

        // Not an error. A window that has never been online has no recorder at
        // all, and "there is no capture" is the honest answer to the question --
        // an error here would make the normal startup state look like a fault.
        if (recorder == nullptr)
        {
            return json{{"running", false},
                        {"messages", 0},
                        {"bytes", 0},
                        {"received", 0},
                        {"retained_span_seconds", 0.0},
                        {"evicted", 0},
                        {"evicted_bytes", 0}};
        }

        const CaptureBuffer& buffer = recorder->buffer();
        return json{
            // False once the window goes offline: the capture is a snapshot of
            // the online session, not a tail that keeps running behind it.
            {"running", recorder->isValid()},
            {"messages", buffer.size()},
            {"bytes", buffer.bytes()},
            {"received", recorder->received()},
            {"retained_span_seconds", buffer.retainedSpanSeconds()},
            // Not cosmetic. A capture silently dropping its head makes the
            // start of a trace look like a publisher that had not started yet
            // -- the same class of lie a recorder dropping samples tells.
            {"evicted", buffer.evicted()},
            {"evicted_bytes", buffer.evictedBytes()}};
    });

    server.registerMethod(
        "scope.review_capture",
        [win](const json& /*params*/) -> MethodResult {
            if (!win->reviewCapture())
            {
                return std::unexpected(badParams(
                    "Nothing has been captured. Go online with scope.set_mode first, or "
                    "load a bag with scope.open_recording."));
            }
            const SourceCaps caps = win->source().caps();
            return json{{"reviewing", true},
                        {"caps", json{{"live", caps.live},
                                      {"seekable", caps.seekable},
                                      {"t_begin", caps.t_begin},
                                      {"t_end", caps.t_end}}}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    server.registerMethod(
        "scope.save_recording",
        [win](const json& params) -> MethodResult {
            const auto path = params.find("path");
            if (path == params.end() || !path->is_string())
            {
                return std::unexpected(
                    badParams("'path' (string) is required -- the bag DIRECTORY to write."));
            }

            if (!win->saveCaptureTo(QString::fromStdString(path->get<std::string>())))
            {
                return std::unexpected(internalError(
                    "Failed to write the capture to '" + path->get<std::string>() + "'."));
            }

            ScopeRecorder* const recorder = win->recorder();
            return json{{"saved", true},
                        {"path", path->get<std::string>()},
                        {"messages", recorder != nullptr ? recorder->buffer().size() : 0}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // ------------------------------------------------------------------ config

    server.registerMethod("scope.panel_get_config", [win](const json& params) -> MethodResult {
        const auto entry = panelFrom(*win, params);
        if (!entry)
        {
            return std::unexpected(entry.error());
        }

        return json{{"panel", entry.value()->id.toStdString()},
                    {"type", panelTypeName(*entry.value()->panel)},
                    {"config", variantToJson(panelConfigOf(*entry.value()->panel))}};
    });

    server.registerMethod("scope.panel_describe_config",
                          [win](const json& params) -> MethodResult {
                              const auto entry = panelFrom(*win, params);
                              if (!entry)
                              {
                                  return std::unexpected(entry.error());
                              }
                              // Describes THIS panel's config. It used to
                              // hardcode describeType<TimeSeriesPanelConfig_t>()
                              // without looking at the panel at all, so every
                              // panel type would have been described as a plot.
                              return json{
                                  {"panel", entry.value()->id.toStdString()},
                                  {"type", panelTypeName(*entry.value()->panel)},
                                  {"schema",
                                   describeVariant(panelConfigOf(*entry.value()->panel))}};
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

            Panel& panel = *entry.value()->panel;

            // Partial but all-or-nothing: only the named fields are touched, and
            // an unknown name is an error rather than a silent no-op. A caller
            // that believes it changed something it did not is the worst
            // outcome here.
            //
            // The patch is applied to whichever config kind this panel actually
            // has, so a field name that belongs to a different panel type is
            // rejected by name rather than quietly ignored.
            panel_config_variant_t updated = panelConfigOf(panel);
            std::vector<std::string> errors;

            std::visit(
                [&patch, &errors](auto& cfg) {
                    using cfg_t = std::decay_t<decltype(cfg)>;
                    if constexpr (std::is_same_v<cfg_t, std::monostate>)
                    {
                        errors.push_back("That panel type has no settable config.");
                    }
                    else
                    {
                        config_codec::applyJson(*patch, cfg, "", errors);
                    }
                },
                updated);

            if (!errors.empty())
            {
                AgentError error = badParams("Config patch rejected.");
                error.data["errors"] = errors;
                return std::unexpected(std::move(error));
            }

            if (!applyPanelConfig(panel, updated))
            {
                return std::unexpected(internalError("Panel refused its own config kind."));
            }

            entry.value()->dock->setWindowTitle(panel.title());
            return json{{"applied", true},
                        {"type", panelTypeName(panel)},
                        {"config", variantToJson(panelConfigOf(panel))}};
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
    // ------------------------------------------------------------------- stats
    //
    // What a panel RECEIVED, as opposed to what it was told to show. A
    // screenshot shows a line or a picture; this says what it is made of, and it
    // is the assertion a test can actually make.
    //
    // Type-agnostic, deliberately. The obvious alternative -- one RPC per panel
    // kind, `scope.sample_stats` then `scope.video_stats` then the next one --
    // reintroduces exactly the per-type list SCOPE_PANEL_TABLE exists to
    // prevent, in the one file that should never need to know how many panel
    // kinds there are.

    server.registerMethod("scope.stats", [win](const json& params) -> MethodResult {
        const auto wanted = params.find("panel");
        if (wanted != params.end() && !wanted->is_string())
        {
            return std::unexpected(badParams("'panel', when given, must be a string."));
        }

        json panels = json::array();
        for (const ScopeWindow::PanelEntry& entry : win->panels())
        {
            if (wanted != params.end() && entry.id.toStdString() != wanted->get<std::string>())
            {
                continue;
            }

            panels.push_back(json{{"panel", entry.id.toStdString()},
                                  {"type", panelTypeName(*entry.panel)},
                                  {"stats", variantToJson(panelStatsOf(*entry.panel))}});
        }

        return json{{"panels", std::move(panels)}};
    });

    // What fields to expect from scope.stats, per panel. Without this a caller
    // has to guess them from a sample that happens to be populated -- and a
    // count that is legitimately absent looks identical to a field that does not
    // exist.
    server.registerMethod("scope.describe_stats", [win](const json& params) -> MethodResult {
        const auto entry = panelFrom(*win, params);
        if (!entry)
        {
            return std::unexpected(entry.error());
        }
        return json{{"panel", entry.value()->id.toStdString()},
                    {"type", panelTypeName(*entry.value()->panel)},
                    {"schema", describeVariant(panelStatsOf(*entry.value()->panel))}};
    });

    // The historical name for the time-series half of scope.stats, kept because
    // docs/scope.md and AGENTS.md both point at it as the first thing to reach
    // for and existing loops use it.
    //
    // Its OUTPUT IS UNCHANGED, including that t_first/t_last/min/max/last are
    // omitted rather than zeroed when nothing has arrived. That is why this
    // builds its JSON by hand instead of going through the reflected codec: the
    // reflected form always emits every field, which is the better answer for a
    // new caller and a silently different one for an old.
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
            for (const trace_stats_t& stats : plot->stats().traces)
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
