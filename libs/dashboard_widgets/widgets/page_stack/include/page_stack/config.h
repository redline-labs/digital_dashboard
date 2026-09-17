#ifndef PAGE_STACK_CONFIG_H
#define PAGE_STACK_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "config_codec/config_limits.h"
#include "dashboard/page_command.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"

// A bus input that changes the page: when `expression` over the messages on
// `zenoh_key` fires (see trigger_edge_t), `action` is applied to this stack.
REFLECT_STRUCT(page_trigger_t,
    (std::string, zenoh_key, "",
        "Zenoh Key", "Topic to watch"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::GrayhillButtons,
        "Schema Type", "Schema of the messages on that topic"),
    (std::string, expression, "",
        "Expression", "Non-zero means true, e.g. bit(buttons1To8, 0)"),
    (trigger_edge_t, edge, trigger_edge_t::rising,
        "Edge", "rising: fire when the expression becomes true (state topics); on_sample: every true message (event topics)"),
    (uint32_t, stale_after_ms, 0,
        "Re-prime After (ms)", "rising only: a gap this long makes the next sample a first sample again; 0 = never"),
    (page_action_t, action, page_action_t::next,
        "Action", "next, prev, go_to or back"),
    (std::string, page, "",
        "Page", "Page name, for go_to")
)

// The pages themselves are not here: they sit beside `config:` in the layout, as
// `pages:`, because they are widgets rather than settings. See widget_page_t in
// dashboard/app_config.h.
REFLECT_STRUCT(PageStackConfig_t,
    (std::string, default_page, "",
        "Default Page", "Page shown at startup; empty = the first"),
    (std::vector<page_trigger_t>, triggers, {},
        "Triggers", "Bus inputs that change the page")
)

inline std::vector<std::string> validate(PageStackConfig_t& cfg)
{
    std::vector<std::string> notes;
    for (page_trigger_t& trigger : cfg.triggers)
    {
        config_codec::limits::clampStaleAfter(trigger.stale_after_ms, "triggers[].stale_after_ms", notes);
    }
    return notes;
}

#endif // PAGE_STACK_CONFIG_H
