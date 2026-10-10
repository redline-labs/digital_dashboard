#ifndef DASHBOARD_WIDGET_TABLE_H
#define DASHBOARD_WIDGET_TABLE_H

// The list of dashboard widgets, in one place.
//
//     X(<enum name>, <C++ class>, <name shown in the editor's palette>)
//
// Generated from it: the widget_type_t enumerators and kWidgetDescriptors (in
// widget_types.h, from the first and third columns), the widget_config_t
// variant, and every compile-time sweep over the widget set (from the second). Those used to be two hand-maintained lists in
// two files, and only one direction of forgetting was diagnosed. Adding to the
// sweep list without adding the enumerator does not compile; adding the
// enumerator without adding to the sweep list builds cleanly and produces a
// widget that exists in the enum, cannot be instantiated, is absent from the
// editor's palette and silently fails to parse from a config.
//
// This header deliberately includes nothing and must stay that way: the enum
// and the descriptors have to be declarable without the widget headers. Naming
// a class here does not create a dependency on it -- widget_types.h never
// expands the second column, and a token that is never expanded is never
// looked up.
//
// Order is significant in one respect: it fixes the alternative order of the
// widget_config_t variant and the order the editor's palette lists widgets.
// Neither is persisted, so reordering is safe, but it is not a no-op.

#define DASHBOARD_WIDGET_TABLE(X) \
    X(static_text,                 StaticTextWidget,          "Static Text") \
    X(road_info,                   RoadInfoWidget,            "Road Info") \
    X(value_readout,               ValueReadoutWidget,        "Value Readout") \
    X(segment_readout,             SegmentReadoutWidget,      "Segment Readout") \
    X(center_bar,                  CenterBarWidget,           "Center Bar") \
    X(mercedes_190e_speedometer,   Mercedes190ESpeedometer,   "Mercedes 190E Speedometer") \
    X(mercedes_190e_tachometer,    Mercedes190ETachometer,    "Mercedes 190E Tachometer") \
    X(mercedes_190e_cluster_gauge, Mercedes190EClusterGauge,  "Mercedes 190E Cluster Gauge") \
    X(sparkline,                   SparklineItem,             "Sparkline") \
    X(background_rect,             BackgroundRectWidget,      "Background Rect") \
    X(mercedes_190e_telltale,      Mercedes190ETelltale,      "Mercedes 190E Telltale") \
    X(motec_c125_tachometer,       MotecC125Tachometer,       "MoTeC C125 Tachometer") \
    X(motec_cdl3_tachometer,       MotecCdl3Tachometer,       "MoTeC CDL3 Tachometer") \
    X(carplay,                     CarPlayWidget,             "CarPlay") \
    X(now_playing,                 NowPlayingWidget,          "Now Playing") \
    X(carplay_nav,                 CarPlayNavWidget,          "CarPlay Navigation") \
    X(map,                         MapWidget,                 "Offline Map") \
    X(page_stack,                  PageStackWidget,           "Page Stack") \
    X(page_button,                 PageButtonWidget,          "Page Button")

#endif // DASHBOARD_WIDGET_TABLE_H
