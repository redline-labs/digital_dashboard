#ifndef DASHBOARD_WIDGET_TYPES_H
#define DASHBOARD_WIDGET_TYPES_H

#include "reflection/reflection.h"
#include "dashboard/widget_table.h"

// Widget type enumeration - extracted to separate header to avoid circular
// dependencies. This can be included by widget headers to declare their
// kWidgetType, which is why neither it nor widget_table.h may include a widget
// header.
//
// Generated from the first column of DASHBOARD_WIDGET_TABLE. The class token in
// the second column is discarded here rather than looked up, so this stays free
// of any dependency on the widgets themselves.
#define DASHBOARD_WIDGET_ENUMERATOR(enum_name, widget_class) enum_name,

// `unknown` is not in the table: it is not a widget, it is the state a
// widget_config_t is in before it has been given one, and what an unrecognised
// `type:` in a config decodes to. widget_registry.h static_asserts that the
// enumerator count matches the table plus this one.
#define DASHBOARD_WIDGET_TYPE_LIST DASHBOARD_WIDGET_TABLE(DASHBOARD_WIDGET_ENUMERATOR) unknown

REFLECT_ENUM(widget_type_t, DASHBOARD_WIDGET_TYPE_LIST)

#undef DASHBOARD_WIDGET_TYPE_LIST
#undef DASHBOARD_WIDGET_ENUMERATOR

#endif // DASHBOARD_WIDGET_TYPES_H
