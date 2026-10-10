#ifndef PAGE_STACK_CONFIG_H
#define PAGE_STACK_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "config_codec/config_limits.h"
#include "dashboard/page_command.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "pub_sub/subscription.h"

// A bus input that changes the page: when `expression` over the messages on
// `zenoh_key` fires (see trigger_edge_t), `action` is applied to this stack.
REFLECT_STRUCT(page_trigger_t,
    // stale_after_ms here is the re-prime gap, for `rising` only: a stream
    // quiet that long makes the next sample a first sample again, so a press
    // held across a publisher restart does not fire twice.
    (pub_sub::subscription_t, source, pub_sub::subscriptionFor(pub_sub::schema_type_t::GrayhillButtons),
        "Source", "Topic and expression to watch; non-zero means true, e.g. bit(buttons1To8, 0). Stale After re-primes a rising trigger"),
    (trigger_edge_t, edge, trigger_edge_t::rising,
        "Edge", "rising: fire when the expression becomes true (state topics); on_sample: every true message (event topics)"),
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

#endif // PAGE_STACK_CONFIG_H
