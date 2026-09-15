#ifndef SCOPE_TABLE_CONFIG_H_
#define SCOPE_TABLE_CONFIG_H_

#include "config_codec/config_limits.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"

#include <cstdint>
#include <string>
#include <vector>

// How a cell renders the number behind it.
//
// A STATE IS NOT A QUANTITY, which is the same argument trace_display_t makes
// for the plot and it lands harder here. A plot at least draws an enum's
// ordinals in the right order; a table cell reading `3` where `iap2` was meant
// is simply the wrong answer, and it is wrong in the one place a reader will
// trust without checking. So enums and bools resolve to their names by default,
// exactly as a lane does, from the same resolver -- see scope/state_names.h.
REFLECT_ENUM(cell_format_t,
    // The state's name for an enum or a bool, a number for anything else. What
    // a drop produces.
    automatic,

    // Force a number. An enum's ordinal is a legitimate thing to want to read
    // -- when comparing against a raw CAN trace, say -- and `automatic` takes
    // it away.
    number,

    // Force a hexadecimal integer, for a status word or a bitmask, where the
    // decimal form of the same number tells you nothing about which bits are
    // set. Rounded to an integer first: 0x3.8 is not a thing anyone wants.
    hex,

    // Force state-name lookup on a field the schema does not mark as an enum --
    // a gear number, a mode. Falls back to the number where there is no name,
    // which is what a state channel with unnamed ordinals has always done.
    state
)

// One row: what to read, and how to print it.
//
// DELIBERATELY NOT signal_binding_t. The first three fields are the same
// binding triple -- the tree's one form, the same one a dashboard gauge uses --
// but a plot's trace also carries `color`, `right_axis` and `display`, and a
// table can honour none of them. Sharing the struct would put three fields in
// this panel's reflected config that the YAML encoder writes, the agent
// interface advertises as settable, `scope.panel_set_config` accepts, and the
// panel then silently ignores. A config that accepts a setting it does not
// implement is worse than two structs that share three field names.
REFLECT_STRUCT(table_row_t,
    (std::string, zenoh_key, "",
        "Zenoh Key", "Zenoh topic key to subscribe to"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::EngineRpm,
        "Schema Type", "Data schema the topic is published with"),
    (std::string, value_expression, "",
        "Value Expression", "Arithmetic over the schema's numeric fields, e.g. 'rpm / 1000'"),
    (std::string, label, "",
        "Label", "Name shown in the first column. Defaults to the expression when empty"),
    (std::string, units, "",
        "Units", "Units shown beside the value, e.g. 'rpm' or 'C'"),
    (cell_format_t, format, cell_format_t::automatic,
        "Format", "Print as a state name, a number, or hexadecimal"),
    // -1 rather than an optional: the reflected codec has no optional, and a
    // sentinel that means "decide from the magnitude" is what the plot's axis
    // labels already do. Anything from 0 upwards is taken literally, so a rate
    // that wants three decimals can have them without the value jumping between
    // widths as it crosses 10.
    (int32_t, decimals, -1,
        "Decimals", "Digits after the decimal point. -1 picks a width from the magnitude, "
                    "the way the plot's axis labels do")
)

// A tabular readout of what every signal is doing RIGHT NOW.
//
// The complement of the plot rather than a lesser version of it: a plot answers
// "what has this been doing", and forty signals of that is forty traces nobody
// can read. This answers "what is everything at, this instant", which is the
// question a pit wall asks and the one a plot is worst at.
REFLECT_STRUCT(TablePanelConfig_t,
    (std::string, title, "Table",
        "Title", "Shown on the panel's title bar"),

    // WHICH INSTANT "NOW" IS, and it is the shared one rather than the newest
    // sample. Under a cursor every panel reads out the same instant -- that is
    // the whole reason the cursor is shared, and it is what makes a table beside
    // a plot beside a video frame one coherent picture rather than three
    // clocks. The plot's legend has always done this; the table is the panel it
    // matters most for.
    //
    // Off means "always the newest sample retained", which is what you want on a
    // live bus when the cursor is parked somewhere from an earlier look.
    (bool, follow_cursor, true,
        "Follow Cursor", "Read every value at the shared cursor, so the table agrees with "
                         "the plots beside it. Off reads the newest sample instead"),

    // A VALUE WITH NO AGE IS A LIE WAITING TO HAPPEN. The last reading from a
    // publisher that died four minutes ago renders identically to one that
    // arrived this millisecond -- that is the failure this column exists for,
    // and it is the one failure a table has that a plot does not, because a plot
    // shows the line stopping.
    (bool, show_age, true,
        "Show Age", "Add a column for how long ago each value arrived"),
    (double, stale_seconds, 2.0,
        "Stale After (s)", "A value older than this is dimmed and marked stale"),

    (bool, show_units, true,
        "Show Units", "Print each row's units beside its value"),

    // COLUMN WIDTHS, IN LOGICAL PIXELS, AND -1 MEANS AUTOMATIC. The same
    // sentinel `decimals` uses above, and for the same reason: a column the user
    // has never touched should keep sizing itself to the panel, and one they
    // dragged should stay exactly where they put it. A plain default cannot
    // express both -- it would freeze every column at a number chosen here,
    // which is what made "airplayHandshake" elide inside a panel with 500 px of
    // empty space in it.
    //
    // The NAME column is deliberately not here. It absorbs whatever the other
    // three leave, so it has no width of its own to store, and giving it one
    // would create a second way to describe the same layout -- two that can
    // disagree, and no rule for which wins.
    //
    // Pixels rather than fractions because pixels are what the user dragged.
    // A fraction survives a resize more gracefully, but it also means a column
    // set to fit "12 ms" stops fitting it the moment the dock narrows, which is
    // the one thing a width the user chose must not do. Widths are clamped
    // against the panel at paint time instead, so a narrow dock squeezes rather
    // than overflows.
    (double, value_width, -1.0,
        "Value Width", "Width of the value column in pixels. -1 sizes it to the panel"),
    (double, units_width, -1.0,
        "Units Width", "Width of the units column in pixels. -1 uses the default"),
    (double, age_width, -1.0,
        "Age Width", "Width of the age column in pixels. -1 uses the default"),

    (std::vector<table_row_t>, rows, {},
        "Rows", "The signals this panel reads out")
)

// Widths below this are not a narrow column, they are a column that cannot show
// anything. The drag clamps to it and so does the loader.
inline constexpr double kMinColumnWidth = 24.0;

// And above this a column is wider than any content it can hold, so the only
// thing it does is push the name column out of the panel.
inline constexpr double kMaxColumnWidth = 400.0;

// Clamped rather than rejected, for the reason the plot's validate() gives: the
// caller is a spin box or a file on disk, and refusing to load a workspace over
// a silly number is worse than loading it with a sane one and saying so.
inline std::vector<std::string> validate(TablePanelConfig_t& cfg)
{
    std::vector<std::string> notes;

    // Zero would mark every row stale the instant it arrived, which is the same
    // as having no staleness marking at all -- and worse, because it looks like
    // a bus that is not delivering. The upper bound is a day, past which the
    // column means nothing either.
    config_codec::limits::clampInto<double>(cfg.stale_seconds, 0.05, 24.0 * 60.0 * 60.0,
                                            "stale_seconds", notes);

    // A column width is either the automatic sentinel or a usable number of
    // pixels. Normalising any negative to -1 rather than clamping it up to the
    // minimum keeps the sentinel's meaning: "-2" is not a 2-pixel column the
    // loader should widen, it is somebody writing "automatic" imprecisely, and
    // silently turning it into a 24-pixel column would be a layout nobody asked
    // for.
    const auto clampWidth = [&notes](double& width, const char* name) {
        if (width < 0.0)
        {
            width = -1.0;
            return;
        }
        config_codec::limits::clampInto<double>(width, kMinColumnWidth, kMaxColumnWidth, name,
                                                notes);
    };
    clampWidth(cfg.value_width, "value_width");
    clampWidth(cfg.units_width, "units_width");
    clampWidth(cfg.age_width, "age_width");

    for (table_row_t& row : cfg.rows)
    {
        // -1 is the sentinel for automatic; below that is nothing, and above 9
        // is past what a double carries.
        config_codec::limits::clampInto<int32_t>(row.decimals, -1, 9, "decimals", notes);
    }

    return notes;
}

#endif  // SCOPE_TABLE_CONFIG_H_
