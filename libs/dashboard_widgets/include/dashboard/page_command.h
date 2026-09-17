// SPDX-License-Identifier: GPL-3.0-or-later
//
// What changing a page_stack's page is called in a config, and where on the bus
// it goes.
//
// Reflection only -- no capnp, no Qt -- so a widget's config.h can hold a
// page_command_t without dragging in the schema, and the pure navigation logic
// can be unit-tested without either. The capnp half is
// dashboard/page_command_publisher.h.
#ifndef DASHBOARD_PAGE_COMMAND_H_
#define DASHBOARD_PAGE_COMMAND_H_

#include "reflection/reflection.h"

#include <string>
#include <string_view>

// `go_to`, not `goto`: the enumerator is a C++ identifier, and that one is a
// keyword. The capnp spelling is goTo.
REFLECT_ENUM(page_action_t,
    next,
    prev,
    go_to,
    back
)

// When a trigger fires.
//
// `rising` is for a topic that reports a STATE -- a keypad's button bitmask, a
// connected flag -- where what matters is the moment it becomes true. Its first
// sample only primes it, so a button held at startup, or a state that is simply
// already true, changes nothing.
//
// `on_sample` is for a topic that reports EVENTS, where every message is one
// occurrence -- CarPlayUiEvent. The first message is an event like any other.
REFLECT_ENUM(trigger_edge_t,
    rising,
    on_sample
)

REFLECT_STRUCT(page_command_t,
    (std::string, target, "",
        "Target", "id of the page_stack to control"),
    (page_action_t, action, page_action_t::next,
        "Action", "next, prev, go_to or back"),
    (std::string, page, "",
        "Page", "Page name; only go_to reads it")
)

namespace dashboard
{

// The topics a page_stack is driven through, derived from its id so there is no
// second setting to keep in step with it.
inline std::string pageCommandKey(std::string_view container_id)
{
    return "dashboard/pages/" + std::string(container_id) + "/command";
}

inline std::string pageStateKey(std::string_view container_id)
{
    return "dashboard/pages/" + std::string(container_id) + "/state";
}

}  // namespace dashboard

#endif  // DASHBOARD_PAGE_COMMAND_H_
