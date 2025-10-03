#ifndef DASHBOARD_EDITOR_WIDGET_REGISTRY_H
#define DASHBOARD_EDITOR_WIDGET_REGISTRY_H

#include <array>
#include <string_view>

#include <QString>
#include <QWidget>

#include "carplay/carplay_widget.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "mercedes_190e_telltales/telltale.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include "motec_c125_tachometer/motec_c125_tachometer.h"
#include "motec_cdl3_tachometer/motec_cdl3_tachometer.h"
#include "static_text/static_text.h"
#include "value_readout/value_readout.h"
#include "background_rect/background_rect.h"

namespace widget_registry
{
// ============================================================================
// Widget Registration List
// ============================================================================
// To add a new widget type:
//   1. Ensure your widget class has these static members:
//      - static constexpr widget_type_t kWidgetType = widget_type_t::your_widget_type;
//      - static constexpr std::string_view kFriendlyName = "User Friendly Name for Widget";
//      - using config_t = YourConfigType
//      - config_t getConfig() const method
//   2. Add ONE line to FOR_EACH_WIDGET below with just the class name
//   3. Add an entry to the widget_type_t enum in dashboard/widget_types.h
//
// Format: X(WidgetClass)
//
// The macro extracts kWidgetType and kFriendlyName from the widget class.
// The widget name string is derived from kWidgetType via reflection::enum_to_string().
// ============================================================================

#define FOR_EACH_WIDGET(X) \
	X(StaticTextWidget) \
	X(ValueReadoutWidget) \
	X(Mercedes190ESpeedometer) \
	X(Mercedes190ETachometer) \
	X(Mercedes190EClusterGauge) \
	X(SparklineItem) \
	X(BackgroundRectWidget) \
	X(Mercedes190ETelltale) \
	X(MotecC125Tachometer) \
	X(MotecCdl3Tachometer) \
	X(CarPlayWidget)


// Generate config_traits specializations from FOR_EACH_WIDGET.
// This is used to go backwards from a widget_config_t to the widget class
// at compile time.
template <typename Config> struct config_traits;

#define WIDGET_TRAITS_SPECIALIZATION(widget_class) \
	template <> \
	struct config_traits<widget_class::config_t> \
	{ \
		static constexpr widget_type_t type = widget_class::kWidgetType; \
		using widget_t = widget_class; \
	};

FOR_EACH_WIDGET(WIDGET_TRAITS_SPECIALIZATION)
#undef WIDGET_TRAITS_SPECIALIZATION

// Generate instantiateWidget function from FOR_EACH_WIDGET
// This is to actually construct a dashboard widget from a widget_config_t enum
// at runtime.
inline QWidget* instantiateWidget(widget_type_t type, QWidget* parent)
{
	QWidget* w = nullptr;
	switch (type)
	{
#define INSTANTIATE_CASE(widget_class) \
		case widget_class::kWidgetType: { \
			widget_class::config_t cfg; \
			w = new widget_class(cfg, parent); \
			break; \
		}
		
		FOR_EACH_WIDGET(INSTANTIATE_CASE)
#undef INSTANTIATE_CASE
		
		case widget_type_t::unknown:
		default:
			w = nullptr;
			break;
	}
	return w;
}

} // namespace widget_registry

#endif // DASHBOARD_EDITOR_WIDGET_REGISTRY_H


