#ifndef PUB_SUB_SUBSCRIPTION_H_
#define PUB_SUB_SUBSCRIPTION_H_

#include <cstdint>
#include <string>
#include <vector>

#include "config_codec/config_limits.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/topic_key.h"
#include "reflection/reflection.h"

namespace pub_sub
{

// One number read off the bus, as a config describes it: which topic, decoded
// as which schema, through which expression, and how long a silence means the
// reading is stale.
//
// Every widget binding, page trigger and scope signal is one of these, held as
// a field named for what it feeds (`rpm:`, `value:`, `latitude:`). It is the
// whole description: whatever a YAML entry sets reaches the subscriber, so
// there is no second struct to choose between and nothing to forward by hand.
//
// An empty zenoh_key means unbound -- nothing subscribes and nothing is
// logged -- which is how a widget nobody has wired up yet is spelled.
REFLECT_STRUCT(subscription_t,
    (topic_key_t, zenoh_key, "",
        "Zenoh Key", "Topic to subscribe to; empty = unbound"),
    (schema_type_t, schema_type, schema_type_t::VehicleSpeed,
        "Schema", "Schema of the messages on that topic"),
    (std::string, expression, "",
        "Expression", "Evaluated against each message, e.g. rpm or bit(buttons1To8, 0)"),
    // How long a gap in the stream means "no data". 0 never reports one.
    (uint32_t, stale_after_ms, 0,
        "Stale After (ms)", "No data for this long means stale; 0 = never")
)

// A subscription whose schema defaults to `schema`: a tachometer's binding
// starts as EngineRpm rather than as whatever the struct's default is. One
// argument, so it can stand as a REFLECT_STRUCT field's initialiser.
inline subscription_t subscriptionFor(schema_type_t schema)
{
    subscription_t subscription;
    subscription.schema_type = schema;
    return subscription;
}

// Do these name THE SAME SIGNAL? Topic, schema and expression; the timeout is
// how a consumer treats the stream, not which stream it is, so two bindings
// that differ only in it share one buffer and one bus subscription.
inline bool sameSignal(const subscription_t& lhs, const subscription_t& rhs)
{
    return lhs.zenoh_key == rhs.zenoh_key && lhs.schema_type == rhs.schema_type &&
           lhs.expression == rhs.expression;
}

// Run for every subscription in a config by config_codec::applyLimits, so no
// config that holds one repeats these.
inline std::vector<std::string> validate(subscription_t& subscription)
{
    std::vector<std::string> notes;
    config_codec::limits::clampStaleAfter(subscription.stale_after_ms, "stale_after_ms", notes);
    if (!subscription.zenoh_key.empty() && subscription.expression.empty())
    {
        // Still subscribed, so the binding goes stale rather than reading zero;
        // said here because nothing else will say why it never shows a value.
        notes.emplace_back("zenoh_key '" + subscription.zenoh_key.str() +
                           "' has no expression, so it will never produce a value");
    }
    return notes;
}

}  // namespace pub_sub

#endif  // PUB_SUB_SUBSCRIPTION_H_
