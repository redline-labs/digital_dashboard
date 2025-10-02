#ifndef DASHBOARD_EDITOR_WIDGET_REGISTRY_H
#define DASHBOARD_EDITOR_WIDGET_REGISTRY_H

#include <array>
#include <string_view>

#include <QString>
#include <QWidget>

#include "dashboard/app_config.h"
#include "carplay/carplay_widget.h"
#include "editor/widget_registry_list.h"

namespace widget_registry
{

struct WidgetInfo
{
	widget_type_t type;
	const std::string_view label;         // user-visible label
	const std::string_view enum_name;     // stable enum string name (for DnD / config)
};

// Generate kAllWidgets array from FOR_EACH_WIDGET
#define WIDGET_INFO_ENTRY(enum_val, widget_class, label) \
	{widget_type_t::enum_val, label, widget_class::kWidgetName},

constexpr std::array kAllWidgets = std::to_array<WidgetInfo>({
	FOR_EACH_WIDGET(WIDGET_INFO_ENTRY)
});
#undef WIDGET_INFO_ENTRY

// Generate config_traits specializations from FOR_EACH_WIDGET
template <typename Config> struct config_traits;

#define WIDGET_TRAITS_SPECIALIZATION(enum_val, widget_class, label) \
	template <> \
	struct config_traits<widget_class::config_t> \
	{ \
		static constexpr widget_type_t type = widget_type_t::enum_val; \
		using widget_t = widget_class; \
	};

FOR_EACH_WIDGET(WIDGET_TRAITS_SPECIALIZATION)
#undef WIDGET_TRAITS_SPECIALIZATION

// Generate instantiateWidget function from FOR_EACH_WIDGET
inline QWidget* instantiateWidget(widget_type_t type, QWidget* parent)
{
	QWidget* w = nullptr;
	switch (type)
	{
#define INSTANTIATE_CASE(enum_val, widget_class, label) \
		case widget_type_t::enum_val: { \
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


