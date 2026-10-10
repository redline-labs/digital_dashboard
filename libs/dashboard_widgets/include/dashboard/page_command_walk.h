#ifndef DASHBOARD_PAGE_COMMAND_WALK_H_
#define DASHBOARD_PAGE_COMMAND_WALK_H_

#include "dashboard/app_config.h"
#include "dashboard/page_command.h"
#include "reflection/reflection.h"

#include <string_view>
#include <type_traits>
#include <variant>

namespace dashboard
{

// Calls `visit(page_command_t&)` for every page command anywhere in `value`:
// a field of that type, in a nested reflected struct, or in a list of them.
//
// Found by TYPE, so a widget that gains a page command is covered with no list
// to update. The editor's page rename used to name page_button and CarPlay's
// return button by hand; a third widget with a command would have kept the old
// page name after a rename and gone dead.
template <typename T, typename Visit>
void forEachPageCommand(T& value, Visit&& visit)
{
    using Plain = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Plain, page_command_t>)
    {
        visit(value);
    }
    else if constexpr (reflection::is_reflected_struct_v<Plain>)
    {
        reflection::visit_fields(value, [&](std::string_view, auto& field, std::string_view) {
            forEachPageCommand(field, visit);
        });
    }
    else if constexpr (reflection::is_std_vector<Plain>::value)
    {
        for (auto& element : value)
        {
            forEachPageCommand(element, visit);
        }
    }
}

// The same over a widget's config, whichever widget it is. Pages are not
// entered: their widgets are widgets of their own.
template <typename Visit>
void forEachPageCommand(widget_config_t& widget, Visit&& visit)
{
    std::visit(
        [&](auto& config) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(config)>, std::monostate>)
            {
                forEachPageCommand(config, visit);
            }
        },
        widget.config);
}

}  // namespace dashboard

#endif  // DASHBOARD_PAGE_COMMAND_WALK_H_
