#ifndef DASHBOARD_WIDGET_TYPES_H
#define DASHBOARD_WIDGET_TYPES_H

#include "reflection/reflection.h"

// Widget type enumeration - extracted to separate header to avoid circular dependencies
// This can be included by widget headers to declare their kWidgetType
REFLECT_ENUM(widget_type_t,
    mercedes_190e_speedometer,
    mercedes_190e_tachometer,
    mercedes_190e_telltale,
    mercedes_190e_cluster_gauge,
    motec_c125_tachometer,
    motec_cdl3_tachometer,
    static_text,
    value_readout,
    background_rect,
    sparkline,
    carplay,
    unknown
)

#endif // DASHBOARD_WIDGET_TYPES_H

