#ifndef SCOPE_TABLE_STATS_H_
#define SCOPE_TABLE_STATS_H_

#include "reflection/reflection.h"

#include <cstdint>
#include <string>
#include <vector>

// What the table actually read, as opposed to what it was told to show.
//
// READ-ONLY, like every stats struct -- see the note in
// scope/panels/time_series/include/time_series/stats.h for why reflection is
// used here and what it is NOT used for. Served through `scope.stats` and
// self-described through `scope.describe_stats`, neither of which knows this
// type exists.
//
// `text` IS THE FIELD THAT MAKES THIS PANEL TESTABLE. Everything else says a
// value arrived; only this says what the cell PRINTS -- and the whole point of
// the format rules is that `3` and `iap2` are the same number and only one of
// them is the right answer. A screenshot would show the text and not the
// ordinal; the two together are what let a test assert the mapping.

REFLECT_STRUCT(row_stats_t,
    (std::string, label, "",
        "Label", "Name shown in the first column"),
    (bool, bound, false,
        "Bound", "Whether the source accepted this signal. False means the expression "
                 "did not compile or the subscription could not be declared"),
    (uint64_t, retained, 0,
        "Retained", "Samples currently in this row's history"),
    (uint64_t, received, 0,
        "Received", "Samples the source has delivered since binding"),
    (uint64_t, dropped, 0,
        "Dropped", "Samples the staging ring lost because the GUI did not drain it in "
                   "time. Anything above zero means a reading was skipped"),

    (bool, has_value, false,
        "Has Value", "False when nothing has arrived at or before the readout instant, "
                     "which makes the fields below meaningless rather than zero"),
    (double, value, 0.0,
        "Value", "The number behind the cell, before formatting"),
    (std::string, text, "",
        "Text", "What the cell prints: the state's name for an enum or a bool, otherwise "
                "the formatted number. THE assertion for enum support -- the value is an "
                "ordinal and this is what a reader actually sees"),
    (bool, state, false,
        "State", "Whether this row resolved to named states rather than to a quantity"),

    (double, sample_t, 0.0,
        "Sample Time (s)", "Source-clock time of the sample being shown"),
    (double, age_seconds, 0.0,
        "Age (s)", "How long before the readout instant that sample arrived"),
    (bool, stale, false,
        "Stale", "The age exceeded the configured limit. The one failure a table has and "
                 "a plot does not: a dead publisher's last reading looks exactly like a "
                 "live one until something says how old it is")
)

REFLECT_STRUCT(TablePanelStats_t,
    // The readout instant every row above was sampled at, so a caller can tell
    // "the cursor is parked in the past" from "every publisher stopped".
    (double, readout_t, 0.0,
        "Readout Time (s)", "The instant the rows were read at: the shared cursor when "
                            "following it, otherwise the view's right edge"),
    (bool, at_cursor, false,
        "At Cursor", "Whether that instant came from the shared cursor rather than from "
                     "the view's right edge"),

    // NOT `signals`. Qt's moc defines that as a macro expanding to `public:`, so
    // a member of that name in a header any Q_OBJECT translation unit sees is a
    // syntax error several lines further down, pointing at the macro invocation
    // rather than at the name. The config calls its list `rows` for the same
    // reason, and `rows` is the better word for a table anyway.
    (std::vector<row_stats_t>, rows, {},
        "Rows", "Per-signal readout state, in the order the panel prints them")
)

#endif  // SCOPE_TABLE_STATS_H_
