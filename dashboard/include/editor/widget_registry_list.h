#ifndef DASHBOARD_EDITOR_WIDGET_REGISTRY_LIST_H
#define DASHBOARD_EDITOR_WIDGET_REGISTRY_LIST_H

// ============================================================================
// SINGLE SOURCE OF TRUTH: Widget Registration List
// ============================================================================
// This header contains ONLY the widget list macro, with no dependencies.
// It can be included by both app_config.h and widget_registry.h without
// creating circular dependencies.
//
// To add a new widget type:
//   1. Add a line to FOR_EACH_WIDGET below
//   2. That's it! Everything else is automatically generated throughout
//      the codebase (widget_registry, properties_panel, selection_frame, etc)
//
// Format: X(enum_value, WidgetClass, "Display Label")
//
// Requirements:
//   - enum_value must match the value in widget_type_t enum
//   - WidgetClass must have:
//       * WidgetClass::config_t typedef
//       * WidgetClass::kWidgetName constant
//       * config_t getConfig() const method
//   - Display Label is shown in the editor palette
// ============================================================================

#define FOR_EACH_WIDGET(X) \
	X(static_text, StaticTextWidget, "Static Text") \
	X(value_readout, ValueReadoutWidget, "Value Readout") \
	X(mercedes_190e_speedometer, Mercedes190ESpeedometer, "Mercedes 190E Speedometer") \
	X(mercedes_190e_tachometer, Mercedes190ETachometer, "Mercedes 190E Tachometer") \
	X(mercedes_190e_cluster_gauge, Mercedes190EClusterGauge, "Mercedes 190E Cluster Gauge") \
	X(sparkline, SparklineItem, "Sparkline") \
	X(background_rect, BackgroundRectWidget, "Background Rect") \
	X(mercedes_190e_telltale, Mercedes190ETelltale, "Mercedes 190E Telltale") \
	X(motec_c125_tachometer, MotecC125Tachometer, "MoTeC C125 Tachometer") \
	X(motec_cdl3_tachometer, MotecCdl3Tachometer, "MoTeC CDL3 Tachometer") \
	X(carplay, CarPlayWidget, "CarPlay")

#endif // DASHBOARD_EDITOR_WIDGET_REGISTRY_LIST_H

