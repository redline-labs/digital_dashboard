#ifndef SCOPE_TIME_SERIES_CONFIG_H_
#define SCOPE_TIME_SERIES_CONFIG_H_

#include "config_codec/config_limits.h"
#include "helpers/color.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"

#include <string>
#include <vector>

// One plotted signal.
//
// The first three fields are the tree's existing binding form -- the same
// triple a dashboard widget uses -- so a signal picked in scope's browser is
// the same thing a gauge binds, and a workspace and a dashboard config describe
// signals identically. Dragging a field out of the browser fills the expression
// in with the bare field name, which is the degenerate "just plot this" case;
// leaving it editable is what makes `temperatureCelsius * 1.8 + 32` work with
// no new UI at all.
REFLECT_STRUCT(signal_binding_t,
    (std::string, zenoh_key, "",
        "Zenoh Key", "Zenoh topic key to subscribe to"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::EngineRpm,
        "Schema Type", "Data schema the topic is published with"),
    (std::string, value_expression, "",
        "Value Expression", "Arithmetic over the schema's numeric fields, e.g. 'rpm / 1000'"),
    (std::string, label, "",
        "Label", "Name shown in the legend. Defaults to the expression when empty"),
    (helpers::Color, color, "#4FC3F7",
        "Color", "Colour of this trace"),
    (std::string, units, "",
        "Units", "Units suffix shown in the legend, e.g. 'rpm' or 'C'"),
    (bool, right_axis, false,
        "Right Axis", "Scale against the right-hand axis instead of the left")
)

REFLECT_STRUCT(TimeSeriesPanelConfig_t,
    (std::string, title, "Plot",
        "Title", "Shown on the panel's title bar"),
    (bool, follow_time_base, true,
        "Follow Shared Time", "Scroll and pause with the rest of the window"),
    (double, window_seconds, 30.0,
        "Window (s)", "Seconds of history shown. Used only when not following the shared time"),
    (bool, autoscale_y, true,
        "Autoscale Y", "Fit the vertical axis to the data currently visible"),
    (double, y_min, 0.0,
        "Y Minimum", "Bottom of the vertical axis when autoscale is off"),
    (double, y_max, 100.0,
        "Y Maximum", "Top of the vertical axis when autoscale is off"),
    (bool, show_grid, true,
        "Show Grid", "Draw grid lines behind the traces"),
    (bool, show_legend, true,
        "Show Legend", "List the traces and their current values"),
    // Named `traces`, not `signals`. Qt's moc defines `signals` as a macro
    // expanding to `public:`, so a member of that name inside a header any
    // Q_OBJECT translation unit sees is a syntax error several lines later,
    // pointing at the macro invocation rather than at the name. `traces` is
    // also the more accurate word for what a plot holds.
    (std::vector<signal_binding_t>, traces, {},
        "Traces", "The signals this panel plots")
)

// Clamped rather than rejected, because the caller is either a spin box or a
// workspace file and refusing to load a config over a silly number is worse
// than loading it with a sane one and saying so. Same reasoning, and the same
// helper, as the dashboard widgets' validate() hooks.
inline std::vector<std::string> validate(TimeSeriesPanelConfig_t& cfg)
{
    std::vector<std::string> notes;
    config_codec::limits::clampInto<double>(cfg.window_seconds, 0.1, 24.0 * 60.0 * 60.0,
                                            "window_seconds", notes);
    // A degenerate range makes the vertical scale a division by zero. Only
    // matters when autoscale is off, but a config is worth fixing either way --
    // turning autoscale off later should not suddenly produce a blank panel.
    config_codec::limits::orderRange(cfg.y_min, cfg.y_max, "the Y range", notes);
    return notes;
}

#endif  // SCOPE_TIME_SERIES_CONFIG_H_
