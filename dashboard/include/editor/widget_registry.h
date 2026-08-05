#ifndef DASHBOARD_EDITOR_WIDGET_REGISTRY_H
#define DASHBOARD_EDITOR_WIDGET_REGISTRY_H

#include <array>
#include <string_view>

#include <QString>
#include <QWidget>

#include "carplay/carplay_widget.h"
#include "carplay_nav/carplay_nav.h"
#include "now_playing/now_playing.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "mercedes_190e_telltales/telltale.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include "motec_c125_tachometer/motec_c125_tachometer.h"
#include "motec_cdl3_tachometer/motec_cdl3_tachometer.h"
#include "static_text/static_text.h"
#include "value_readout/value_readout.h"
#include "segment_readout/segment_readout.h"
#include "center_bar/center_bar.h"
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
//   2. Add ONE line to DASHBOARD_WIDGET_TABLE in dashboard/widget_table.h
//   3. Call add_dashboard_widget() in your widget's CMakeLists.txt, and
//      add_subdirectory() in dashboard/widgets/CMakeLists.txt
//
// Config types need no registration. Anything declared with REFLECT_STRUCT or
// REFLECT_ENUM converts to and from YAML on its own, nested structs and enums
// included -- see the constrained convert<> specializations in app_config.h.
//
// DASHBOARD_WIDGET_TABLE drives every sweep over the widget set, and the
// widget_type_t enumerators come from the same table, so the two cannot drift
// apart. Everything else is derived from the widget class: the widget name
// string comes from kWidgetType via reflection::enum_to_string(), and the
// widget_config_t variant alternatives from the table.
//
// A sweep macro takes both columns -- X(enum_name, WidgetClass) -- and most
// ignore the first. Threading a one-argument macro through the table instead
// would need an adapter holding the caller's macro in a fixed name, which is
// less readable than an unused parameter.
// ============================================================================

// The enum and the sweeps are generated from the same table, so they cannot
// drift apart on their own. This pins that they were in fact generated: it
// fires if an enumerator is ever added to widget_types.h by hand, which is the
// one way back to the silent failure the table exists to prevent.
#define DASHBOARD_WIDGET_COUNT_ONE(enum_name, widget_class) +1
static_assert(
	reflection::enum_traits<widget_type_t>::names().size()
		== static_cast<std::size_t>(1 DASHBOARD_WIDGET_TABLE(DASHBOARD_WIDGET_COUNT_ONE)),
	"widget_type_t does not match DASHBOARD_WIDGET_TABLE. The enum is generated "
	"from the table, so add widgets to widget_table.h rather than to "
	"widget_types.h. The 1 is `unknown`, which is deliberately not in the table.");
#undef DASHBOARD_WIDGET_COUNT_ONE


// Generate config_traits specializations from the widget table.
// This is used to go backwards from a widget_config_t to the widget class
// at compile time.
template <typename Config> struct config_traits;

#define WIDGET_TRAITS_SPECIALIZATION(enum_name, widget_class) \
	template <> \
	struct config_traits<widget_class::config_t> \
	{ \
		static constexpr widget_type_t type = widget_class::kWidgetType; \
		using widget_t = widget_class; \
	};

DASHBOARD_WIDGET_TABLE(WIDGET_TRAITS_SPECIALIZATION)
#undef WIDGET_TRAITS_SPECIALIZATION

// There is no instantiateWidget() here any more.
//
// It built a widget straight from a default-constructed config, which meant the
// editor had a second construction path that skipped the range checking in
// widget_factory::createWidgetFromConfig -- so a config the dashboard clamped
// was previewed unclamped, and the editor was the optimistic one. Both apps now
// go through widget_factory. To build a widget of a given type with its own
// defaults, ask for default_widget_config(type) and hand that to the factory.

} // namespace widget_registry

#endif // DASHBOARD_EDITOR_WIDGET_REGISTRY_H


