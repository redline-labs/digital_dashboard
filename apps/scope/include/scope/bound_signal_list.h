#ifndef SCOPE_BOUND_SIGNAL_LIST_H_
#define SCOPE_BOUND_SIGNAL_LIST_H_

#include "scope/data_source.h"
#include "scope/sample_ring.h"
#include "scope/state_names.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace scope
{

// The shared mechanics of a panel that binds a LIST of signals: the plot's
// traces and the table's rows are the same ~120 lines with the element renamed,
// and both files carried the same data-loss story in a comment because the same
// bug was fixed twice. Hoisted so the third panel of this shape gets the rule
// for free -- and so the rule lives in ONE place:
//
//   RECONCILING WITH A NEW CONFIG KEEPS THE BUFFER OF EVERY ENTRY THAT IS
//   STILL THERE. Rebuilding all of them was a data-loss bug, not an
//   inefficiency: adding a signal gave every OTHER entry a brand-new empty
//   buffer, and while the view is paused the readout instant is frozen in the
//   past, where the new buffers have nothing. Every table cell read "--", the
//   plot went blank, and nothing logged it, because from the panel's point of
//   view it had just bound successfully.
//
// An Entry provides: `binding` (with zenoh_key / schema_type /
// value_expression), `buffer` (shared_ptr<SignalBuffer>), `handle`
// (SignalHandle), `bound` (bool) and `states` (StateNames). Everything beyond
// that -- a colour, a lane flag, a cell format -- is the panel's own, applied
// through the presentation callback.

// Do these two bindings name THE SAME SIGNAL? The binding triple and nothing
// else: a colour, a label, a units suffix, an axis, a display mode or a cell
// format is presentation, and changing one must not cost the entry its history.
//
// The same triple SignalKey is built from, deliberately -- this is the identity
// the source issues a handle against, so two entries that compare equal here
// are two the source cannot tell apart.
template <typename A, typename B>
bool sameBoundSignal(const A& lhs, const B& rhs)
{
    return lhs.zenoh_key == rhs.zenoh_key && lhs.schema_type == rhs.schema_type &&
           lhs.value_expression == rhs.value_expression;
}

// Build one entry and bind it: the buffer, the SignalKey, the bind, and the
// state-name resolution every list panel does identically. State names are
// resolved HERE rather than at paint time because resolution reads the schema
// registry and builds a JSON description -- fine once per binding, absurd at
// 30 Hz. The capacity constants stay the caller's: a plot retains 600k points,
// a table 120k, and that difference is a per-panel decision.
template <typename Entry, typename Binding>
std::unique_ptr<Entry> makeBoundSignal(DataSource& source, const Binding& binding,
                                       double history_seconds, std::size_t max_points,
                                       std::size_t staging_capacity,
                                       const std::string& panel_title)
{
    auto entry = std::make_unique<Entry>();
    entry->binding = binding;
    entry->buffer =
        std::make_shared<SignalBuffer>(history_seconds, max_points, staging_capacity);

    SignalKey key;
    key.zenoh_key = binding.zenoh_key;
    key.schema_type = binding.schema_type;
    key.value_expression = binding.value_expression;

    entry->handle = source.bind(key, entry->buffer);
    entry->bound = entry->handle != kInvalidSignal;

    entry->states = resolveStateNames(binding.schema_type, binding.value_expression);

    if (!entry->bound)
    {
        // Already logged in detail by the evaluator; this says which panel.
        SPDLOG_WARN("Panel '{}': signal '{}' on '{}' could not be bound.", panel_title,
                    binding.value_expression, binding.zenoh_key);
    }

    return entry;
}

// Bring `entries` into line with `wanted`, keeping the buffer of every entry
// still present (see the data-loss story at the top of this header).
//
// `makeEntry(binding)` builds a fresh, bound entry; `applyPresentation(entry)`
// re-reads the presentation half of a binding that stayed. A matched entry is
// MOVED out of the previous list, so a second binding naming the same signal
// -- which only a hand-edited workspace can produce -- gets its own entry
// rather than stealing this one's buffer and leaving two entries sharing it.
// Whatever goes unclaimed is genuinely gone, and its subscription with it.
template <typename Entry, typename Binding, typename MakeEntry, typename ApplyPresentation>
void syncBoundSignals(std::vector<std::unique_ptr<Entry>>& entries,
                      const std::vector<Binding>& wanted, DataSource& source,
                      MakeEntry&& makeEntry, ApplyPresentation&& applyPresentation)
{
    std::vector<std::unique_ptr<Entry>> previous;
    previous.swap(entries);
    entries.reserve(wanted.size());

    for (const Binding& binding : wanted)
    {
        const auto match = std::find_if(previous.begin(), previous.end(),
                                        [&binding](const std::unique_ptr<Entry>& entry) {
                                            return entry &&
                                                   sameBoundSignal(entry->binding, binding);
                                        });

        if (match == previous.end())
        {
            entries.push_back(makeEntry(binding));
            continue;
        }

        std::unique_ptr<Entry> entry = std::move(*match);
        entry->binding = binding;  // Presentation may have changed; the signal did not.
        applyPresentation(*entry);
        entries.push_back(std::move(entry));
    }

    for (const std::unique_ptr<Entry>& leftover : previous)
    {
        if (leftover && leftover->handle != kInvalidSignal)
        {
            source.release(leftover->handle);
        }
    }
}

// Release every entry's handle against `source` and clear the list. For the
// destructor and for rebindAll() -- the two cases where a buffer genuinely
// cannot be carried over (a different source issued the handles, or the
// retention the buffers were built with changed).
template <typename Entry>
void releaseBoundSignals(std::vector<std::unique_ptr<Entry>>& entries, DataSource& source)
{
    for (const std::unique_ptr<Entry>& entry : entries)
    {
        if (entry->handle != kInvalidSignal)
        {
            source.release(entry->handle);
        }
    }
    entries.clear();
}

}  // namespace scope

#endif  // SCOPE_BOUND_SIGNAL_LIST_H_
