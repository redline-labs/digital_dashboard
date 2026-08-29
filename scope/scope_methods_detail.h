#ifndef SCOPE_SCOPE_METHODS_DETAIL_H_
#define SCOPE_SCOPE_METHODS_DETAIL_H_

// INTERNAL to the scope.* method registration -- deliberately beside the .cpp
// files rather than under include/scope/, because nothing outside them may
// depend on it. registerScopeMethods() (scope_methods.cpp) is the public face;
// the verbs themselves are registered by one file per area, mirroring the
// grouping docs/scope.md documents:
//
//   scope_methods_panels.cpp  -- composition: panels, signals, the browser
//   scope_methods_time.cpp    -- the shared clock: time_base, density
//   scope_methods_source.cpp  -- what is behind the panels: source, mode,
//                                capture, recordings
//   scope_methods_config.cpp  -- reflected config, settings, workspaces, stats
//
// Everything here is shared plumbing: the JSON/variant helpers that keep the
// verbs panel-type-agnostic, and the registrar that wraps every handler in a
// flushSeek() so "seek then read" is exact (see FlushedRegistrar).

#include "scope/data_source.h"
#include "scope/panel_registry.h"
#include "scope/recorded_source.h"
#include "scope/scope_recorder.h"
#include "scope/scope_window.h"
#include "scope/settings.h"
#include "scope/signal_browser.h"
#include "scope/time_base.h"

#include "table/table_panel.h"
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
namespace methods_detail
{

using agent_control::AgentError;
using agent_control::badParams;
using agent_control::json;
using agent_control::MethodResult;

inline AgentError internalError(std::string message)
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

inline json variantToJson(const auto& variant)
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

inline json describeVariant(const auto& variant)
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

inline std::string panelTypeName(const Panel& panel)
{
    return std::string(reflection::enum_traits<panel_type_t>::to_string(panel.panelType()));
}

// Every panel-addressed method needs this, and all of them should fail the same
// way: naming a panel that is not there is a caller error, and the reply lists
// what there is so the next call can be right.
inline std::expected<ScopeWindow::PanelEntry*, AgentError> panelFrom(ScopeWindow& window,
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

inline json describePanel(const ScopeWindow::PanelEntry& entry)
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

    // The plot's traces, served from the reflected codec rather than a
    // hand-written per-field block -- this was the last qobject_cast in the
    // RPC layer, and the hand copy had already drifted (it never learned
    // right_axis or display). Kept under the historical "traces" key because
    // callers read it; every OTHER panel's full config is one
    // scope.panel_get_config away, which serves the same reflected JSON.
    if (const auto* plot = qobject_cast<const TimeSeriesPanel*>(entry.panel))
    {
        out["traces"] = config_codec::toJson(plot->getConfig().traces);
    }

    return out;
}

inline json candidateToJson(const BindingCandidate& candidate)
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

// Registers a scope.* method with the seek flush every handler needs. Seeks
// coalesce to the render tick (TimeBase::flushSeek), which is right for
// gestures and wrong for an agent that sets the view and immediately reads
// sample_stats -- it would see the buffers from BEFORE its own seek, up to a
// frame stale. Flushing in the registrar rather than per-method means no
// method can be forgotten, and a flush with nothing pending is one branch.
class FlushedRegistrar
{
  public:
    FlushedRegistrar(agent_control::AgentServer& server, ScopeWindow& window) :
        server_(&server), window_(&window)
    {
    }

    void operator()(std::string name, agent_control::AgentServer::Method handler,
                    agent_control::AgentServer::MethodKind kind =
                        agent_control::AgentServer::MethodKind::kReadOnly) const
    {
        ScopeWindow* const win = window_;
        server_->registerMethod(std::move(name),
                                [win, fn = std::move(handler)](const nlohmann::json& params)
                                    -> MethodResult
                                {
                                    win->timeBase().flushSeek();
                                    return fn(params);
                                },
                                kind);
    }

  private:
    agent_control::AgentServer* server_;
    ScopeWindow* window_;
};

// One registration function per area; each lives in its own file.
void registerPanelMethods(const FlushedRegistrar& registerFlushed, ScopeWindow& window);
void registerTimeMethods(const FlushedRegistrar& registerFlushed, ScopeWindow& window);
void registerSourceMethods(const FlushedRegistrar& registerFlushed, ScopeWindow& window);
void registerConfigMethods(const FlushedRegistrar& registerFlushed, ScopeWindow& window);

}  // namespace methods_detail
}  // namespace scope

#endif  // SCOPE_SCOPE_METHODS_DETAIL_H_
