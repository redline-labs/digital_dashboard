#include "scope_methods_detail.h"

namespace scope
{
namespace methods_detail
{

// The reflected surfaces: per-panel config, per-user settings,
// workspaces, and what every panel has received.
void registerConfigMethods(const FlushedRegistrar& registerFlushed, ScopeWindow& window)
{
    ScopeWindow* const win = &window;


    // ------------------------------------------------------------------ config

    registerFlushed("scope.panel_get_config", [win](const json& params) -> MethodResult {
        const auto entry = panelFrom(*win, params);
        if (!entry)
        {
            return std::unexpected(entry.error());
        }

        return json{{"panel", entry.value()->id.toStdString()},
                    {"type", panelTypeName(*entry.value()->panel)},
                    {"config", variantToJson(panelConfigOf(*entry.value()->panel))}};
    });

    registerFlushed("scope.panel_describe_config",
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

    registerFlushed(
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

    // ---------------------------------------------------------------- settings

    // Read with no params, write with {"tilesets": [{"name":..., "path":...}]}.
    //
    // The HEADLESS path, and the one every test uses. File ▸ Settings… is a
    // second front end onto the same ScopeWindow::setSettings(); a modal dialog
    // under --mcp has nobody to dismiss it.
    //
    // Writing replaces the whole list rather than merging: a caller that wanted
    // to remove a tileset has no way to say so through a merge, and a partial
    // write that silently kept an old entry is the kind of thing that shows up
    // later as a map drawn from the wrong archive.
    registerFlushed(
        "scope.settings",
        [win](const json& params) -> MethodResult {
            const auto tilesets = params.find("tilesets");
            if (tilesets != params.end())
            {
                if (!tilesets->is_array())
                {
                    return std::unexpected(badParams("'tilesets' must be an array."));
                }

                scope_settings_t updated;
                for (const json& entry : *tilesets)
                {
                    if (!entry.is_object())
                    {
                        return std::unexpected(
                            badParams("Each tileset must be an object with 'name' and 'path'."));
                    }
                    const auto name = entry.find("name");
                    const auto path = entry.find("path");
                    if (name == entry.end() || !name->is_string() || path == entry.end() ||
                        !path->is_string())
                    {
                        return std::unexpected(
                            badParams("Each tileset needs 'name' and 'path' (both strings)."));
                    }
                    scope_tileset_t tileset;
                    tileset.name = name->get<std::string>();
                    tileset.path = path->get<std::string>();
                    updated.tilesets.push_back(std::move(tileset));
                }

                if (!win->setSettings(updated))
                {
                    return std::unexpected(internalError(
                        "Failed to write '" + win->settingsFilePath().toStdString() + "'."));
                }
            }

            json tileset_list = json::array();
            for (const scope_tileset_t& tileset : win->settings().tilesets)
            {
                tileset_list.push_back({{"name", tileset.name}, {"path", tileset.path}});
            }

            // The notes are the useful half of a read: a duplicate or unnamed
            // tileset is not an error but it does mean a panel will not find
            // what it asked for, and nothing else reports that.
            json notes = json::array();
            for (const std::string& note : checkTilesets(win->settings()))
            {
                notes.push_back(note);
            }

            return json{{"path", win->settingsFilePath().toStdString()},
                        {"tilesets", std::move(tileset_list)},
                        {"notes", std::move(notes)}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // --------------------------------------------------------------- workspace

    registerFlushed(
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

    registerFlushed(
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

    registerFlushed("scope.stats", [win](const json& params) -> MethodResult {
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
    registerFlushed("scope.describe_stats", [win](const json& params) -> MethodResult {
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
    registerFlushed("scope.sample_stats", [win](const json& params) -> MethodResult {
        json panels = json::array();

        for (const ScopeWindow::PanelEntry& entry : win->panels())
        {
            if (params.contains("panel") &&
                entry.id.toStdString() != params.value("panel", std::string{}))
            {
                continue;
            }

            // Not `signals`: Qt's moc defines that as a macro expanding to
            // `public:`, so even a local of that name is a syntax error.
            json signal_list = json::array();

            if (auto* plot = qobject_cast<TimeSeriesPanel*>(entry.panel))
            {
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
            }
            else if (auto* table = qobject_cast<TablePanel*>(entry.panel))
            {
                // A table's rows are the same kind of signal a plot's traces
                // are, and this method is the first debugging verb the docs
                // reach for -- a panel it skips looks like a panel receiving
                // nothing. Video and map keep their richer, shape-specific
                // stats under scope.stats.
                for (const row_stats_t& stats : table->stats().rows)
                {
                    json out;
                    out["label"] = stats.label;
                    out["bound"] = stats.bound;
                    out["retained"] = stats.retained;
                    out["received"] = stats.received;
                    out["dropped"] = stats.dropped;
                    out["has_data"] = stats.has_value;
                    if (stats.has_value)
                    {
                        out["t_last"] = stats.sample_t;
                        out["last"] = stats.value;
                    }
                    signal_list.push_back(std::move(out));
                }
            }
            else
            {
                continue;
            }

            panels.push_back(json{{"panel", entry.id.toStdString()}, {"signals", std::move(signal_list)}});
        }

        return json{{"panels", std::move(panels)}};
    });
}

}  // namespace methods_detail
}  // namespace scope
