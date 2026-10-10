#include "pub_sub/sample_check.h"

#include "pub_sub/schema_layout.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace pub_sub
{

static_assert(kNoLayout == 0, "judgeSample() spells kNoLayout as 0");

namespace
{
// True the first time only, for a once-per-gate message.
bool first(std::atomic<bool>& said)
{
    return !said.exchange(true, std::memory_order_relaxed);
}
}  // namespace

SampleGate::SampleGate(std::string keyexpr, std::string_view schema_name, std::uint64_t layout) :
    keyexpr_(std::move(keyexpr)), schema_name_(schema_name), layout_(layout)
{
}

bool SampleGate::admit(std::string_view published_name,
                       std::optional<std::uint64_t> published_layout)
{
    switch (judgeSample(schema_name_, layout_, published_name, published_layout))
    {
        case SampleVerdict::accept:
            return true;

        case SampleVerdict::unnamed:
            if (first(said_unnamed_))
            {
                SPDLOG_DEBUG("'{}' names no schema; decoding it as the expected '{}'", keyexpr_,
                             schema_name_);
            }
            return true;

        case SampleVerdict::wrong_schema:
            if (first(said_wrong_schema_))
            {
                SPDLOG_ERROR("Dropping samples on '{}': they are published as '{}' but read as "
                             "'{}'. Decoding them would produce plausible wrong values; fix the "
                             "schema in the config.",
                             keyexpr_, published_name, schema_name_);
            }
            return false;

        case SampleVerdict::wrong_layout:
            if (first(said_wrong_layout_))
            {
                SPDLOG_ERROR("Dropping samples on '{}': they are '{}' written against a different "
                             "revision of that schema (publisher {:016x}, this build {:016x}). "
                             "Rebuild both sides from the same schemas.",
                             keyexpr_, schema_name_, published_layout.value_or(0), layout_);
            }
            return false;
    }
    return false;
}

void SampleGate::refusePartialWord(std::size_t bytes)
{
    if (first(said_partial_word_))
    {
        SPDLOG_WARN("Dropping samples on '{}': a {}-byte payload is not a whole number of capnp "
                    "words",
                    keyexpr_, bytes);
    }
}

void SampleGate::refuseMalformed(std::string_view why)
{
    if (first(said_malformed_))
    {
        SPDLOG_WARN("Dropping a malformed {} message on '{}': {}", schema_name_, keyexpr_, why);
    }
}

}  // namespace pub_sub
