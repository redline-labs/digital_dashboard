#ifndef SCOPE_TIME_SERIES_STATS_H_
#define SCOPE_TIME_SERIES_STATS_H_

#include "reflection/reflection.h"

#include <cstdint>
#include <string>
#include <vector>

// What this panel actually received, as opposed to what it was asked to plot.
//
// REFLECTED, AND READ-ONLY. Reflection is used here for the same two things it
// is used for everywhere else -- JSON one way, and a self-description for
// tooling -- and for neither of the things it is used for in a *config*: nothing
// edits a stats struct, and the defaults below exist only because REFLECT_STRUCT
// takes one per field. It is a different type from panel_config_variant_t
// precisely so that applyPanelConfig() cannot be handed one by accident.
//
// The counts are uint64_t rather than std::size_t deliberately. On darwin
// size_t is `unsigned long` and uint64_t is `unsigned long long` -- distinct
// types to the reflection machinery, even at the same width -- so spelling the
// fixed-width one keeps the JSON identical across platforms.

REFLECT_STRUCT(trace_stats_t,
    (std::string, label, "",
        "Label", "Name shown in the legend"),
    (bool, bound, false,
        "Bound", "Whether the source accepted this signal. False means the expression "
                 "did not compile or the subscription could not be declared"),
    (uint64_t, retained, 0,
        "Retained", "Samples currently in the plotted history"),
    (uint64_t, received, 0,
        "Received", "Samples the source has delivered since binding"),
    (uint64_t, dropped, 0,
        "Dropped", "Samples the staging ring lost because the GUI did not drain it in "
                   "time. Anything above zero means the trace is lying about the data"),
    (bool, lane, false,
        "Lane", "Drawn as a state lane rather than as a line. True for an enum or a bool "
                "unless the trace overrides it"),
    (bool, has_data, false,
        "Has Data", "False when nothing has arrived yet, which makes the four fields "
                    "below meaningless rather than zero"),
    (double, t_first, 0.0,
        "First (s)", "Time of the oldest retained sample, on the source's clock"),
    (double, t_last, 0.0,
        "Last (s)", "Time of the newest retained sample, on the source's clock"),
    (double, min, 0.0,
        "Minimum", "Smallest value in the retained history"),
    (double, max, 0.0,
        "Maximum", "Largest value in the retained history"),
    (double, last, 0.0,
        "Latest", "Value of the newest retained sample")
)

REFLECT_STRUCT(TimeSeriesPanelStats_t,
    // NOT `signals`. Qt's moc defines that as a macro expanding to `public:`, so
    // a member of that name in a header any Q_OBJECT translation unit sees is a
    // syntax error several lines further down, pointing at the macro invocation
    // rather than at the name. The config calls its list `traces` for the same
    // reason, and `traces` is the better word anyway.
    (std::vector<trace_stats_t>, traces, {},
        "Traces", "Per-signal buffer state, in the order the panel plots them")
)

#endif  // SCOPE_TIME_SERIES_STATS_H_
