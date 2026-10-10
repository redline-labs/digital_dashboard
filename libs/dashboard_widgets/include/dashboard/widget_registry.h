#ifndef DASHBOARD_WIDGET_REGISTRY_H
#define DASHBOARD_WIDGET_REGISTRY_H

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
#include "map/map_widget.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include "motec_c125_tachometer/motec_c125_tachometer.h"
#include "motec_cdl3_tachometer/motec_cdl3_tachometer.h"
#include "road_info/road_info.h"
#include "static_text/static_text.h"
#include "value_readout/value_readout.h"
#include "segment_readout/segment_readout.h"
#include "center_bar/center_bar.h"
#include "background_rect/background_rect.h"
#include "page_stack/page_stack.h"
#include "page_button/page_button.h"

namespace widget_registry
{
// ============================================================================
// Widget Registration List
// ============================================================================
// To add a new widget type:
//   1. Give the widget class `using config_t = YourConfigType;` and a
//      `const config_t& getConfig() const`, and construct it from a config_t.
//   2. Add ONE row to DASHBOARD_WIDGET_TABLE in dashboard/widget_table.h:
//      the type name, the class, and the name the palette shows.
//   3. Include its header below, and add its case to validate_widget's switch
//      in app_config.cpp (-Wswitch-enum names the missing one).
//   4. Give it a CMakeLists calling add_dashboard_widget(); widgets/CMakeLists
//      picks up every directory on its own.
//
// Config types need no registration. Anything declared with REFLECT_STRUCT or
// REFLECT_ENUM converts to and from YAML on its own, nested structs and enums
// included -- see the constrained convert<> specializations in config_yaml.h.
//
// A sweep macro takes all three columns -- X(enum_name, WidgetClass,
// friendly_name) -- and most ignore some. Anything that needs only the names
// reads kWidgetDescriptors (widget_types.h) instead of sweeping.
// ============================================================================

// The enum and the sweeps are generated from the same table, so they cannot
// drift apart on their own. This pins that they were in fact generated: it
// fires if an enumerator is ever added to widget_types.h by hand, which is the
// one way back to the silent failure the table exists to prevent.
#define DASHBOARD_WIDGET_COUNT_ONE(enum_name, widget_class, friendly_name) +1
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

#define WIDGET_TRAITS_SPECIALIZATION(enum_name, widget_class, friendly_name) \
	template <> \
	struct config_traits<widget_class::config_t> \
	{ \
		static constexpr widget_type_t type = widget_type_t::enum_name; \
		using widget_t = widget_class; \
	};

DASHBOARD_WIDGET_TABLE(WIDGET_TRAITS_SPECIALIZATION)
#undef WIDGET_TRAITS_SPECIALIZATION

} // namespace widget_registry

#endif // DASHBOARD_WIDGET_REGISTRY_H


