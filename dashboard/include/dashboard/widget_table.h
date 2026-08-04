#ifndef DASHBOARD_WIDGET_TABLE_H
#define DASHBOARD_WIDGET_TABLE_H

// The list of dashboard widgets, in one place.
//
//     X(<enum name>, <C++ class>)
//
// Two things are generated from this: the widget_type_t enumerators (from the
// first column, in widget_types.h) and every compile-time sweep over the
// widget set (from the second). Those used to be two hand-maintained lists in
// two files, and only one direction of forgetting was diagnosed. Adding to the
// sweep list without adding the enumerator does not compile; adding the
// enumerator without adding to the sweep list builds cleanly and produces a
// widget that exists in the enum, cannot be instantiated, is absent from the
// editor's palette and silently fails to parse from a config.
//
// This header deliberately includes nothing and must stay that way. The enum
// has to be declarable without the widget headers, because the widget headers
// include it to declare their kWidgetType. Naming a class here does not create
// a dependency on it -- widget_types.h expands only the first column, and a
// token that is never expanded is never looked up.
//
// Order is significant in one respect: it fixes the alternative order of the
// widget_config_t variant and the order the editor's palette lists widgets.
// Neither is persisted, so reordering is safe, but it is not a no-op.

#define DASHBOARD_WIDGET_TABLE(X) \
    X(static_text,                 StaticTextWidget) \
    X(value_readout,               ValueReadoutWidget) \
    X(segment_readout,             SegmentReadoutWidget) \
    X(center_bar,                  CenterBarWidget) \
    X(mercedes_190e_speedometer,   Mercedes190ESpeedometer) \
    X(mercedes_190e_tachometer,    Mercedes190ETachometer) \
    X(mercedes_190e_cluster_gauge, Mercedes190EClusterGauge) \
    X(sparkline,                   SparklineItem) \
    X(background_rect,             BackgroundRectWidget) \
    X(mercedes_190e_telltale,      Mercedes190ETelltale) \
    X(motec_c125_tachometer,       MotecC125Tachometer) \
    X(motec_cdl3_tachometer,       MotecCdl3Tachometer) \
    X(carplay,                     CarPlayWidget) \
    X(now_playing,                 NowPlayingWidget) \
    X(carplay_nav,                 CarPlayNavWidget)

#endif // DASHBOARD_WIDGET_TABLE_H
