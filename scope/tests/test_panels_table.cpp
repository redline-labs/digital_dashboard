// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "test_panels_common.h"

// The table panel: readout instants, staleness, state names, and the
// rebind-keeps-history rule it was the first to expose.

namespace panel_tests
{


table_row_t phaseRow()
{
    table_row_t row;
    row.zenoh_key = "nodes/carplay/session";
    row.schema_type = pub_sub::schema_type_t::CarPlaySessionState;
    row.value_expression = "phase";
    return row;
}

// A table takes exactly what a plot takes -- and that is not an accident of
// implementation, it is the point. The fields a plot reads worst (enums, bools)
// are the ones a table reads best, so the same drop has to reach both.
void testATableAcceptsWhatAPlotAccepts()
{
    StubSource source;

    std::unique_ptr<scope::Panel> table =
        scope::createPanel(scope::default_panel_config(scope::panel_type_t::table), source);
    expect(table != nullptr, "the table panel constructed");
    if (table == nullptr)
    {
        return;
    }

    expect(table->acceptsBinding(numericField()), "a table takes a numeric field");
    expect(!table->acceptsBinding(videoTopic()), "a table declines a whole topic");
    expect(!table->acceptsBinding(otherTopic()),
           "including one whose schema it could otherwise read");

    scope::BindingCandidate enum_field = numericField();
    enum_field.zenoh_key = "nodes/carplay/session";
    enum_field.schema_name = "CarPlaySessionState";
    enum_field.field_name = "phase";
    enum_field.type_category = "enum";
    expect(table->acceptsBinding(enum_field),
           "an ENUM field is accepted -- the field this panel exists to read");

    expect(table->addBinding(enum_field), "and binds");
    expect(!table->addBinding(enum_field),
           "a second identical row is declined rather than read out twice");
}

// THE READING ITSELF: the newest sample at or before the readout instant, held
// rather than interpolated, with the age that says whether it still means
// anything.
void testATableReadsTheValueAtTheInstant()
{
    StubSource source;

    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());
    scope::TablePanel table(cfg, source);

    expect(source.buffers.size() == 1, "the row bound one signal");
    feed(source, 0, {{97.0, 1000.0}, {98.0, 3000.0}, {99.5, 4200.0}});

    const TablePanelStats_t stats = table.stats();
    expect(stats.rows.size() == 1, "one row is reported");
    if (stats.rows.size() != 1)
    {
        return;
    }

    expect(stats.readout_t == 100.0, "with no cursor the readout instant is the source's clock");
    expect(!stats.at_cursor, "and it did not come from a cursor");
    expect(stats.rows[0].has_value, "the row has a value");
    expect(stats.rows[0].value == 4200.0,
           "which is the NEWEST sample at or before the instant, not the first or an average");
    expect(stats.rows[0].text == "4200", "printed without decimals it does not have");
    expect(std::abs(stats.rows[0].age_seconds - 0.5) < 1e-9,
           "the age is measured from the readout instant to the sample");
    expect(!stats.rows[0].stale, "half a second old is not stale");
    expect(stats.rows[0].retained == 3, "and all three samples are still retained");
}

// A value that arrives AFTER the instant being read is not shown. Interpolating
// or reaching forward would print a number that had not happened yet, which on a
// scrubbed recording is the difference between reading a cause and reading its
// effect.
void testATableWillNotReadForwards()
{
    StubSource source;

    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());
    scope::TablePanel table(cfg, source);

    feed(source, 0, {{100.5, 4200.0}});

    const TablePanelStats_t stats = table.stats();
    expect(stats.rows.size() == 1 && !stats.rows[0].has_value,
           "a sample from after the readout instant is not read out");
    expect(stats.rows.size() == 1 && stats.rows[0].retained == 1,
           "though it is retained, and will be read once the instant reaches it");
}

// A DEAD PUBLISHER'S LAST READING LOOKS EXACTLY LIKE A LIVE ONE. That is the one
// failure mode a table has and a plot does not -- a plot draws the line stopping
// -- so the age is not a nicety, it is what stops this panel from lying.
void testATableMarksAStaleReading()
{
    StubSource source;

    TablePanelConfig_t cfg;
    cfg.stale_seconds = 2.0;
    cfg.rows.push_back(rpmRow());
    cfg.rows.push_back(rpmRow());
    cfg.rows[1].value_expression = "oilPressurePsi";
    cfg.rows[1].label = "oil";
    scope::TablePanel table(cfg, source);

    feed(source, 0, {{99.9, 4200.0}});   // Fresh.
    feed(source, 1, {{70.0, 45.0}});     // Thirty seconds old.

    const TablePanelStats_t stats = table.stats();
    expect(stats.rows.size() == 2, "both rows report");
    if (stats.rows.size() != 2)
    {
        return;
    }

    expect(!stats.rows[0].stale, "a fresh reading is not stale");
    expect(stats.rows[1].has_value && stats.rows[1].stale,
           "a reading older than the limit is still SHOWN, and marked stale");
    expect(std::abs(stats.rows[1].age_seconds - 30.0) < 1e-9,
           "with the age that says exactly how far behind it is");
}

// ENUM SUPPORT, which is the whole reason a cell is better than a lane for a
// state: `3` and `iap2` are the same number and only one of them is an answer.
void testATableSpellsAStateByName()
{
    StubSource source;

    TablePanelConfig_t cfg;

    cfg.rows.push_back(phaseRow());                     // Automatic -> named.
    cfg.rows.push_back(phaseRow());
    cfg.rows[1].value_expression = "micActive";         // A bool, also named.
    cfg.rows.push_back(phaseRow());
    cfg.rows[2].format = cell_format_t::number;         // Forced to the ordinal.
    cfg.rows[2].decimals = 0;
    cfg.rows.push_back(phaseRow());
    cfg.rows[3].value_expression = "phase * 2";         // Arithmetic: a number.
    cfg.rows[3].decimals = 0;
    cfg.rows.push_back(phaseRow());
    cfg.rows[4].value_expression = "mainWidthPx";       // Plain integer...
    cfg.rows[4].format = cell_format_t::state;          // ...forced to a state.
    cfg.rows[4].decimals = 0;

    scope::TablePanel table(cfg, source);
    expect(source.buffers.size() == 5, "all five rows bound");
    if (source.buffers.size() != 5)
    {
        return;
    }

    feed(source, 0, {{99.0, 3.0}});
    feed(source, 1, {{99.0, 1.0}});
    feed(source, 2, {{99.0, 3.0}});
    feed(source, 3, {{99.0, 6.0}});
    feed(source, 4, {{99.0, 3.0}});

    const std::vector<row_stats_t> rows = table.stats().rows;
    expect(rows.size() == 5, "five rows report");
    if (rows.size() != 5)
    {
        return;
    }

    expect(rows[0].state && rows[0].text == "iap2",
           "an enum reads as its enumerant name, not as its ordinal");
    expect(rows[0].value == 3.0, "with the ordinal still available underneath");

    expect(rows[1].state && rows[1].text == "true", "a bool reads as true/false");

    expect(!rows[2].state && rows[2].text == "3",
           "format: number forces the ordinal, because reading it against a raw CAN "
           "trace is a legitimate thing to want");

    expect(!rows[3].state && rows[3].text == "6",
           "an enum with arithmetic applied has left the enum's domain, so naming it "
           "would be a lie");

    expect(rows[4].state && rows[4].text == "3",
           "format: state on a field with no names falls back to the number rather "
           "than to nothing");
}

// Hex, for the status words and bitmasks whose decimal form says nothing about
// which bits are set.
void testATableCanPrintHex()
{
    StubSource source;

    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());
    cfg.rows[0].format = cell_format_t::hex;
    scope::TablePanel table(cfg, source);

    feed(source, 0, {{99.0, 255.0}});
    expect(table.stats().rows.at(0).text == "0xFF", "a hex cell prints an unsigned pattern");

    // Rounded to an integer first: 0x3.8 is not a thing anyone wants to read.
    feed(source, 0, {{99.5, 3.5}});
    expect(table.stats().rows.at(0).text == "0x4", "a fractional value is rounded, not truncated "
                                                   "into nonsense");
}

// UNDER A CURSOR EVERY PANEL READS THE SAME INSTANT. That is the whole reason
// the cursor is shared, and a table is where it pays off most: three plots and a
// table all answering "what was everything at t = 98?" at once.
void testATableReadsTheSharedCursor()
{
    StubSource source;

    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());
    scope::TablePanel table(cfg, source);

    scope::TimeBase base(source);
    table.setTimeBase(&base);

    feed(source, 0, {{97.0, 1000.0}, {98.0, 3000.0}, {99.5, 4200.0}});

    expect(table.stats().rows.at(0).value == 4200.0,
           "with no cursor the table reads the view's right edge");

    base.setCursor(98.4);
    const TablePanelStats_t at_cursor = table.stats();
    expect(at_cursor.at_cursor, "the readout instant came from the cursor");
    expect(at_cursor.readout_t == 98.4, "and is the cursor's instant");
    expect(at_cursor.rows.at(0).value == 3000.0,
           "so the value is the one held at that instant, not the newest one");
    expect(std::abs(at_cursor.rows.at(0).age_seconds - 0.4) < 1e-9,
           "and the age is measured back from the cursor, not from now");

    // Off means "always the newest", which is what you want on a live bus with a
    // cursor left parked from an earlier look.
    TablePanelConfig_t unfollowing = cfg;
    unfollowing.follow_cursor = false;
    table.applyConfig(unfollowing);

    // A config change rebinds, which builds a NEW buffer -- the history the old
    // one held is gone, honestly, because a rebound signal has no past. So the
    // samples go into the buffer the source has just issued.
    feed(source, source.buffers.size() - 1, {{97.0, 1000.0}, {98.0, 3000.0}, {99.5, 4200.0}});
    expect(!table.stats().at_cursor && table.stats().rows.at(0).value == 4200.0,
           "follow_cursor off ignores the cursor and reads the newest sample");

    base.setCursor(std::nullopt);
}

// REGRESSION, and it is the nastiest kind: a panel that reported success while
// destroying the data it was showing.
//
// Reported from a live session. Attached to the bus with a few signals reading
// correctly, pause, then add one more signal -- and EVERY row in the table went
// to "--". Resuming brought the values back, but only from the resume point
// forwards; everything before it stayed blank for good.
//
// The cause was addBinding() rebuilding all the rows, so the existing ones got
// brand-new empty buffers and their history was gone. While the view is paused
// the readout instant is frozen in the past, and a fresh buffer has nothing at
// or before it -- so every row correctly reported "nothing here", and the panel
// looked dead. Nothing logged it: from the panel's point of view it had just
// bound successfully.
void testAddingASignalDoesNotWipeTheOthersHistory()
{
    StubSource source;

    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());
    scope::TablePanel table(cfg, source);

    scope::TimeBase base(source);
    table.setTimeBase(&base);

    feed(source, 0, {{97.0, 1000.0}, {99.0, 4200.0}});
    expect(table.stats().rows.at(0).value == 4200.0, "the first signal reads correctly live");

    // Pause. The readout instant freezes at the source's clock, which is where
    // every sample so far already is.
    base.setMode(scope::TimeBase::Mode::Paused);
    expect(table.stats().readout_t == 100.0, "the instant froze where the view was");

    // And now add a second signal, which is the entire report.
    scope::BindingCandidate second = numericField();
    second.field_name = "oilPressurePsi";
    second.type_category = "float";
    expect(table.addBinding(second), "the second signal is added");

    const TablePanelStats_t after = table.stats();
    expect(after.rows.size() == 2, "both rows are present");
    if (after.rows.size() != 2)
    {
        return;
    }

    expect(after.rows[0].has_value && after.rows[0].value == 4200.0,
           "THE BUG: the existing row still reads its value while paused");
    expect(after.rows[0].retained == 2, "because it kept its buffer rather than being rebuilt");

    // The new row genuinely has nothing at the frozen instant -- it was not
    // subscribed then, and inventing a reading for it would be the lie the
    // "--" exists to avoid.
    expect(!after.rows[1].has_value, "the NEW row honestly has nothing at an instant it predates");

    // And the source was asked for exactly one new subscription, with nothing
    // released. This is the assertion that fails loudly if anyone reintroduces
    // a wholesale rebind: it would show two binds for the first signal.
    expect(source.bound.size() == 2, "one bind per signal, not one per signal per edit");
    expect(source.released.empty(), "and nothing was released, because nothing went away");
}

// The same rule through every other route a row can change.
void testOnlyTheChangedRowsAreRebound()
{
    StubSource source;

    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());
    cfg.rows.push_back(rpmRow());
    cfg.rows[1].value_expression = "oilPressurePsi";
    cfg.rows[1].label = "oil";

    scope::TablePanel table(cfg, source);
    feed(source, 0, {{99.0, 4200.0}});
    feed(source, 1, {{99.0, 45.0}});
    expect(source.bound.size() == 2, "two signals bound");

    // Presentation only: a label, a format, a column width. Nothing rebinds.
    TablePanelConfig_t relabelled = cfg;
    relabelled.rows[0].label = "engine speed";
    relabelled.rows[0].decimals = 1;
    table.applyConfig(relabelled);

    expect(source.bound.size() == 2, "renaming a row does not rebind it");
    expect(source.released.empty(), "and releases nothing");
    expect(table.stats().rows.at(0).retained == 1, "so its history survives");
    expect(table.stats().rows.at(0).label == "engine speed", "with the new label in effect");
    expect(table.stats().rows.at(0).text == "4200.0", "and the new decimals");

    // Removing the FIRST row must not disturb the second.
    expect(table.removeBinding(0), "the first row is removed");
    expect(source.released.size() == 1, "exactly one subscription is released");
    expect(source.bound.size() == 2, "and the survivor is not rebound");
    expect(table.stats().rows.size() == 1 && table.stats().rows.at(0).retained == 1,
           "so the surviving row keeps its history");

    // Repointing a row IS a new binding, and the old one has to go.
    TablePanelConfig_t repointed = table.getConfig();
    repointed.rows[0].value_expression = "coolantTempC";
    table.applyConfig(repointed);

    expect(source.bound.size() == 3, "a changed expression binds anew");
    expect(source.released.size() == 2, "and releases the subscription it replaced");
    expect(table.stats().rows.at(0).retained == 0, "with the history honestly gone");
}

// The ordering rule, for a third panel kind. A handle means nothing to a source
// that did not issue it, so every panel must release against the OLD source
// before it repoints -- and this is the seam where a new panel gets it wrong.
void testTablePanelReleasesBeforeRepointing()
{
    StubSource first;
    SeekableStub second;

    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());
    scope::TablePanel table(cfg, first);

    expect(first.bound.size() == 1, "the first source issued a handle");
    const scope::SignalHandle issued = first.next_handle - 1;

    table.rebindTo(second);

    expect(first.released.size() == 1 && first.released.front() == issued,
           "the handle was released against the source that issued it");
    expect(second.released.empty(), "and not against the new one, which never issued it");
    expect(second.bound.size() == 1, "the row rebound on the new source");
}

// THE OTHER HALF OF THE BINDING SEAM, driven off the table rather than off a
// list of types written here -- so it fails the moment a new panel kind can be
// given a binding it cannot give back.
//
// The failure it exists to prevent is not a crash. The context menu simply would
// not offer "Remove signal", and `scope.remove_signal` would answer "that panel
// has no removable signals" for a panel that plainly has one. Both were true of
// the video panel until bindingLabels()/removeBinding() became virtual.
void testEveryPanelKindCanGiveABindingBack()
{
    for (const scope::PanelTypeInfo& info : scope::availablePanelTypes())
    {
        const std::string name(info.name);
        StubSource source;

        std::unique_ptr<scope::Panel> panel =
            scope::createPanel(scope::default_panel_config(info.type), source);
        expect(panel != nullptr, name + ": constructed");
        if (panel == nullptr)
        {
            continue;
        }

        expect(panel->bindingLabels().empty(), name + ": a fresh panel holds no bindings");
        expect(!panel->removeBinding(0), name + ": and removing from it fails cleanly");

        // Whichever of the two candidate shapes this panel takes. Every panel
        // kind has to accept one of them or it could never be given anything.
        scope::BindingCandidate candidate = numericField();
        if (!panel->acceptsBinding(candidate))
        {
            candidate = videoTopic();
        }
        expect(panel->acceptsBinding(candidate), name + ": takes a field or a topic");
        expect(panel->addBinding(candidate), name + ": and binds it");

        expect(panel->bindingLabels().size() == 1,
               name + ": reports the binding it was given");
        expect(!panel->bindingLabels().front().isEmpty(),
               name + ": under a name a human can pick out of a menu");

        expect(!panel->removeBinding(9), name + ": an index it does not have fails cleanly");
        expect(panel->removeBinding(0), name + ": and the one it does have is removed");
        expect(panel->bindingLabels().empty(), name + ": leaving it holding nothing");
    }
}

// The same regression, in the plot. Same cause, different symptom: instead of
// every cell reading "--", every line already on the plot is blanked and starts
// again from the instant the signal was added.
//
// This one also makes the class header true. It has always promised that
// "signals that are unchanged keep their history rather than being torn down and
// restarted", while applyConfig() rebuilt everything.
void testAddingATraceDoesNotWipeTheOthersHistory()
{
    StubSource source;

    TimeSeriesPanelConfig_t cfg;
    signal_binding_t first;
    first.zenoh_key = "vehicle/engine/rpm";
    first.schema_type = pub_sub::schema_type_t::EngineRpm;
    first.value_expression = "rpm";
    cfg.traces.push_back(first);

    scope::TimeSeriesPanel plot(cfg, source);
    source.buffers.at(0)->push({99.0, 4200.0});
    source.buffers.at(0)->drain(100.0);
    expect(plot.stats().traces.at(0).retained == 1, "the first trace has a sample");

    scope::BindingCandidate second = numericField();
    second.field_name = "oilPressurePsi";
    second.type_category = "float";
    expect(plot.addBinding(second), "a second signal is added");

    expect(plot.stats().traces.at(0).retained == 1,
           "THE BUG: the existing trace keeps its history when another is added");
    expect(source.bound.size() == 2, "one bind per signal, not one per signal per edit");
    expect(source.released.empty(), "and nothing was released");

    // Recolouring is presentation. It must not cost the trace its data -- and
    // the new colour still has to take effect.
    TimeSeriesPanelConfig_t recoloured = plot.getConfig();
    recoloured.traces[0].color = helpers::Color("#FF0000");
    recoloured.traces[0].label = "engine speed";
    plot.applyConfig(recoloured);

    expect(plot.stats().traces.at(0).retained == 1, "recolouring keeps the history too");
    expect(source.bound.size() == 2, "because it does not rebind");
    expect(plot.stats().traces.at(0).label == "engine speed", "and the new label is in effect");
}

void testStatsReportUnboundSignals()
{
    // A binding that fails must be visible, not just absent: an empty trace and
    // a broken one look identical on screen.
    StubSource source;
    TimeSeriesPanelConfig_t config;

    signal_binding_t binding;
    binding.zenoh_key = "vehicle/engine/rpm";
    binding.schema_type = pub_sub::schema_type_t::EngineRpm;
    binding.value_expression = "rpm";
    binding.label = "rpm";
    config.traces.push_back(binding);

    scope::TimeSeriesPanel plot(config, source);
    const std::vector<trace_stats_t> stats = plot.stats().traces;

    expect(stats.size() == 1, "there is one signal to report on");
    if (!stats.empty())
    {
        expect(stats[0].label == "rpm", "the label is reported");
        expect(stats[0].bound, "a signal the source accepted reports as bound");
        expect(!stats[0].has_data, "a signal with no samples reports no data rather than zeros");
    }
}

void runTableTests()
{
    testAddingATraceDoesNotWipeTheOthersHistory();
    testStatsReportUnboundSignals();
    testATableAcceptsWhatAPlotAccepts();
    testATableReadsTheValueAtTheInstant();
    testATableWillNotReadForwards();
    testATableMarksAStaleReading();
    testATableSpellsAStateByName();
    testATableCanPrintHex();
    testATableReadsTheSharedCursor();
    testAddingASignalDoesNotWipeTheOthersHistory();
    testOnlyTheChangedRowsAreRebound();
    testTablePanelReleasesBeforeRepointing();
    testEveryPanelKindCanGiveABindingBack();
}

}  // namespace panel_tests
