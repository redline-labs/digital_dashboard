#ifndef SCOPE_PANEL_TABLE_H_
#define SCOPE_PANEL_TABLE_H_

// The single source of truth for what kinds of panel exist.
//
//     X(enum_name, PanelClass)
//
// Everything else derives from this list: the panel_type_t enum, the config
// variant, default_panel_config(), the Panels menu, the YAML decoder chain, and
// the agent interface's list of what it will accept. Adding a panel type is one
// line here plus the panel's own directory -- the same arrangement
// dashboard/include/dashboard/widget_table.h has, and for the same reason: a
// list repeated in four places is a list that will disagree with itself.
//
// This header MUST include nothing and MUST stay that way. scope/panel_types.h
// expands only column 1 to build the enum, and an unexpanded token is never
// looked up, so a panel's own header can include the enum without a cycle back
// to itself. widget_table.h carries the same warning after learning it the hard
// way.
//
// A video panel, a tabular readout, an XY plot: each is one line here. That is
// the extension path, and it is why Panel's interface talks about accepting a
// BindingCandidate rather than about signals -- see panel.h.
#define SCOPE_PANEL_TABLE(X)        \
    X(time_series, TimeSeriesPanel) \
    X(video, VideoPanel)            \
    X(table, TablePanel)            \
    X(map, MapPanel)

#endif  // SCOPE_PANEL_TABLE_H_
