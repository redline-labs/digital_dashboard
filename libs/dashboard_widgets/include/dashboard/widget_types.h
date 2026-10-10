#ifndef DASHBOARD_WIDGET_TYPES_H
#define DASHBOARD_WIDGET_TYPES_H

#include <string>
#include <string_view>

#include "reflection/reflection.h"
#include "dashboard/widget_table.h"

// Widget type enumeration and descriptors - kept apart from the widget headers
// so anything can name a widget type without pulling every widget in, which is
// why neither this nor widget_table.h may include a widget header.
//
// Generated from the first column of DASHBOARD_WIDGET_TABLE. The class token in
// the second column is discarded here rather than looked up, so this stays free
// of any dependency on the widgets themselves.
#define DASHBOARD_WIDGET_ENUMERATOR(enum_name, widget_class, friendly_name) enum_name,

// `unknown` is not in the table: it is not a widget, it is the state a
// widget_config_t is in before it has been given one, and what an unrecognised
// `type:` in a config decodes to. widget_registry.h static_asserts that the
// enumerator count matches the table plus this one.
#define DASHBOARD_WIDGET_TYPE_LIST DASHBOARD_WIDGET_TABLE(DASHBOARD_WIDGET_ENUMERATOR) unknown

REFLECT_ENUM(widget_type_t, DASHBOARD_WIDGET_TYPE_LIST)

#undef DASHBOARD_WIDGET_TYPE_LIST
#undef DASHBOARD_WIDGET_ENUMERATOR

// What every widget type is called, for anything that needs a list or a name
// rather than the class: the palette, the agent interface's known types, the
// properties panel's heading, a validator's "expected one of". One table read,
// rather than a sweep over the widget headers in each of those places.
struct widget_descriptor_t
{
    widget_type_t type;
    std::string_view name;           // the `type:` string in a layout
    std::string_view friendly_name;  // what a person is shown
};

#define DASHBOARD_WIDGET_DESCRIPTOR(enum_name, widget_class, friendly_name) \
    widget_descriptor_t{widget_type_t::enum_name, #enum_name, friendly_name},
inline constexpr widget_descriptor_t kWidgetDescriptors[] = {
    DASHBOARD_WIDGET_TABLE(DASHBOARD_WIDGET_DESCRIPTOR)
};
#undef DASHBOARD_WIDGET_DESCRIPTOR

// The descriptor for `type`, or nullptr for `unknown`.
constexpr const widget_descriptor_t* widgetDescriptor(widget_type_t type)
{
    for (const widget_descriptor_t& descriptor : kWidgetDescriptors)
    {
        if (descriptor.type == type)
        {
            return &descriptor;
        }
    }
    return nullptr;
}

// "static_text, road_info, ..." -- what an unknown `type:` is told to pick from.
inline std::string knownWidgetTypeNames()
{
    std::string known;
    for (const widget_descriptor_t& descriptor : kWidgetDescriptors)
    {
        if (!known.empty()) known += ", ";
        known += descriptor.name;
    }
    return known;
}

#endif // DASHBOARD_WIDGET_TYPES_H
