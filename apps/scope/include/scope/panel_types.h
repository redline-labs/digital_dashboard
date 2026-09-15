#ifndef SCOPE_PANEL_TYPES_H_
#define SCOPE_PANEL_TYPES_H_

#include "reflection/reflection.h"
#include "scope/panel_table.h"

namespace scope
{

// Panel type enumeration, in its own header so that a panel's own header can
// declare its kPanelType without a cycle: this reaches the table but never a
// panel class. Neither this nor panel_table.h may include a panel header.
//
// Generated from the first column of SCOPE_PANEL_TABLE. The class token in the
// second column is discarded here rather than looked up, which is what keeps
// this free of any dependency on the panels themselves.
#define SCOPE_PANEL_ENUMERATOR(enum_name, panel_class) enum_name,

// `unknown` is not in the table: it is not a panel type, it is the state a
// panel_entry_t is in before it has been given one, and what an unrecognised
// `type:` in a workspace decodes to. panel_registry.h static_asserts that the
// enumerator count matches the table plus this one.
#define SCOPE_PANEL_TYPE_LIST SCOPE_PANEL_TABLE(SCOPE_PANEL_ENUMERATOR) unknown

REFLECT_ENUM(panel_type_t, SCOPE_PANEL_TYPE_LIST)

#undef SCOPE_PANEL_TYPE_LIST
#undef SCOPE_PANEL_ENUMERATOR

}  // namespace scope

#endif  // SCOPE_PANEL_TYPES_H_
