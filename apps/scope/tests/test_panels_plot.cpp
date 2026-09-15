// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "test_panels_common.h"

// The time-series panel: what it accepts, how bindings keep their
// history, lanes, colours, and per-trace stats.

namespace panel_tests
{

// ------------------------------------------------------------------- accepting

void testAPlotAcceptsOnlyNumericFields()
{
    StubSource source;
    TimeSeriesPanelConfig_t config;
    scope::TimeSeriesPanel plot(config, source);

    expect(plot.acceptsBinding(numericField()), "a numeric field is accepted");

    scope::BindingCandidate text = numericField();
    text.type_category = "text";
    expect(!plot.acceptsBinding(text), "a text field is declined");

    scope::BindingCandidate topic = numericField();
    topic.field_name.clear();
    expect(!plot.acceptsBinding(topic),
           "a whole topic is declined -- that is what another panel type would take");

    scope::BindingCandidate no_key = numericField();
    no_key.zenoh_key.clear();
    expect(!plot.acceptsBinding(no_key), "a candidate with no topic is declined");

    // ENUM belongs here now. It reads as its ordinal, which is what makes a
    // state channel plottable at all -- and until it did, every enum on this bus
    // was unbindable by anything.
    for (const char* category : {"int", "uint", "float", "bool", "enum"})
    {
        scope::BindingCandidate numeric = numericField();
        numeric.type_category = category;
        expect(plot.acceptsBinding(numeric),
               std::string("a '") + category + "' field is accepted");
    }

    // "list" is absent: whether a list is plottable depends on its ELEMENTS, so
    // it is covered separately below rather than being flatly declined.
    for (const char* category : {"data", "struct", "void", "other", ""})
    {
        scope::BindingCandidate other = numericField();
        other.type_category = category;
        expect(!plot.acceptsBinding(other),
               std::string("a '") + category + "' field is declined");
    }
}

void testAddingASignalBindsIt()
{
    StubSource source;
    TimeSeriesPanelConfig_t config;
    scope::TimeSeriesPanel plot(config, source);

    expect(plot.addBinding(numericField()), "adding an acceptable candidate succeeds");
    expect(plot.getConfig().traces.size() == 1, "the trace is recorded in the config");
    expect(source.bound.size() == 1, "the signal was bound on the source");

    if (!source.bound.empty())
    {
        expect(source.bound[0].zenoh_key == "vehicle/engine/rpm", "bound on the right topic");
        expect(source.bound[0].value_expression == "rpm",
               "the degenerate expression is the bare field name");
    }
}

// A list is plottable exactly when its ELEMENTS are, and the expression a drop
// produces has to name an element -- `values` alone is a vector, which cannot
// compile. Getting either half wrong binds something that never produces a
// sample, which on screen is a flat empty trace: indistinguishable from a
// publisher that has not started.
void testAListIsAcceptedOnItsElementType()
{
    StubSource source;
    scope::TimeSeriesPanel plot(TimeSeriesPanelConfig_t{}, source);

    scope::BindingCandidate numeric_list;
    numeric_list.zenoh_key = "nodes/motec/pdm_output_current";
    numeric_list.schema_name = "MotecPdmOutputCurrent";
    numeric_list.field_name = "values";
    numeric_list.type_category = "list";
    numeric_list.element_category = "float";
    numeric_list.has_fixed_length = true;
    expect(plot.acceptsBinding(numeric_list), "a List(Float32) of declared length is accepted");
    expect(numeric_list.defaultExpression() == "values[0]",
           "and a drop produces an INDEXED expression, not the bare list name");

    scope::BindingCandidate enum_list = numeric_list;
    enum_list.element_category = "enum";
    expect(plot.acceptsBinding(enum_list), "a List(SomeEnum) is accepted");

    scope::BindingCandidate text_list = numeric_list;
    text_list.element_category = "text";
    expect(!plot.acceptsBinding(text_list), "a List(Text) is declined");

    scope::BindingCandidate untyped_list = numeric_list;
    untyped_list.element_category = "";
    expect(!plot.acceptsBinding(untyped_list),
           "a list whose element type is unknown is declined rather than guessed at");

    // AND THE LENGTH HAS TO BE DECLARED. Without it the evaluator refuses the
    // binding, so accepting the drop here would mean a panel that says yes and
    // then shows nothing -- which reads as a broken app rather than as a field
    // that cannot be plotted.
    scope::BindingCandidate variable_list = numeric_list;
    variable_list.has_fixed_length = false;
    expect(!plot.acceptsBinding(variable_list),
           "a list with no declared length is declined");

    // A scalar's default expression is still the bare field name.
    scope::BindingCandidate scalar = numericField();
    expect(scalar.defaultExpression() == "rpm",
           "a scalar field's default expression is unchanged");
}

// An enum or a bool is drawn as a state lane, not as a line, and a line's
// autoscale must not see it.
//
// Both halves matter. A bool rendered as a line is a signal that appears to
// spend half its time at 0.5; an enum's ordinals are labels rather than
// quantities, so a line sloping between two of them draws a transition that
// never happened. And an enum whose ordinals run 0..7 sharing an autoscale with
// rpm flattens the rpm trace against the top of the plot -- so getting this
// wrong ruins the trace BESIDE it, not just the state channel.
void testEnumsAndBoolsBecomeLanes()
{
    StubSource source;

    TimeSeriesPanelConfig_t cfg;

    signal_binding_t enum_trace;
    enum_trace.zenoh_key = "nodes/carplay/session";
    enum_trace.schema_type = pub_sub::schema_type_t::CarPlaySessionState;
    enum_trace.value_expression = "phase";
    cfg.traces.push_back(enum_trace);

    signal_binding_t bool_trace = enum_trace;
    bool_trace.value_expression = "micActive";
    cfg.traces.push_back(bool_trace);

    signal_binding_t numeric_trace = enum_trace;
    numeric_trace.value_expression = "mainWidthPx";
    cfg.traces.push_back(numeric_trace);

    // An enum with arithmetic done to it is a NUMBER, not a state. Labelling it
    // with enumerant names would be a lie, so it stays a line.
    signal_binding_t derived = enum_trace;
    derived.value_expression = "phase * 2";
    cfg.traces.push_back(derived);

    scope::TimeSeriesPanel plot(cfg, source);
    const std::vector<trace_stats_t> stats = plot.stats().traces;
    expect(stats.size() == 4, "all four traces bound");
    if (stats.size() != 4)
    {
        return;
    }

    expect(stats[0].lane, "an enum field is drawn as a lane");
    expect(stats[1].lane, "a bool field is drawn as a lane");
    expect(!stats[2].lane, "a numeric field stays a line");
    expect(!stats[3].lane, "an enum with arithmetic applied is a number, so it stays a line");
}

// The override, both ways. `automatic` is a default, not a decree: plotting gear
// against rpm on the value axis is a legitimate thing to want, and so is forcing
// a lane onto a small integer that is really a state but which nothing in the
// schema marks as one.
void testTheDisplayOverrideWinsBothWays()
{
    StubSource source;

    TimeSeriesPanelConfig_t cfg;

    signal_binding_t forced_line;
    forced_line.zenoh_key = "nodes/carplay/session";
    forced_line.schema_type = pub_sub::schema_type_t::CarPlaySessionState;
    forced_line.value_expression = "phase";
    forced_line.display = trace_display_t::line;
    cfg.traces.push_back(forced_line);

    signal_binding_t forced_lane = forced_line;
    forced_lane.value_expression = "mainWidthPx";
    forced_lane.display = trace_display_t::lane;
    cfg.traces.push_back(forced_lane);

    scope::TimeSeriesPanel plot(cfg, source);
    const std::vector<trace_stats_t> stats = plot.stats().traces;
    expect(stats.size() == 2, "both traces bound");
    if (stats.size() != 2)
    {
        return;
    }

    expect(!stats[0].lane, "display: line forces an enum onto the value axis");
    expect(stats[1].lane, "display: lane forces a plain integer into a lane");
}

// A list with a declared length expands into one draggable candidate per
// element.
//
// The alternative designs both lie. Guessing a count offers rows that do not
// exist, and binding one produces no reading -- on screen a flat empty trace,
// indistinguishable from a dead publisher. Peeking at a live message makes the
// browser show different things depending on whether traffic happened to be
// flowing. The $fixedLength annotation makes it a fact about the schema.
//
// Checked through candidates(), which is both what the tree renders and what
// `scope.browser` reports -- so the agent interface gets the element rows too,
// rather than a second discovery path that could disagree.
void testAListWithADeclaredLengthExpands()
{
    StubSource source;
    source.available.push_back(
        scope::TopicInfo{"nodes/motec/pdm_output_current", "MotecPdmOutputCurrent", true});

    // Non-zero, so the first sync actually rebuilds: syncFromDirectory skips
    // when the revision has not moved, and a stub sitting at 0 never moves.
    source.revision = 1;

    scope::SignalBrowser browser(source);

    const std::vector<scope::BindingCandidate> candidates = browser.candidates();

    int elements = 0;
    bool saw_seventh = false;
    bool saw_bare_list = false;
    for (const scope::BindingCandidate& candidate : candidates)
    {
        if (candidate.field_name != "values")
        {
            continue;
        }
        if (candidate.element_index >= 0)
        {
            ++elements;
            if (candidate.element_index == 7)
            {
                saw_seventh = true;
                expect(candidate.defaultExpression() == "values[7]",
                       "a drop on the eighth element produces values[7], with no hand editing");
                expect(candidate.isNumeric(), "and a float element is plottable");
                expect(candidate.type_category == "list" &&
                           candidate.element_category == "float",
                       "carrying both the list and element categories");
            }
        }
        else
        {
            saw_bare_list = true;
            expect(candidate.defaultExpression() == "values[0]",
                   "the list row itself still falls back to element 0");
        }
    }

    // 32, from the annotation -- not a guess and not a peek.
    expect(elements == 32, "the list expanded into one candidate per element");
    expect(saw_seventh, "the eighth element is among them");
    expect(saw_bare_list, "and the list field itself is still offered");
}

void testAddingTheSameSignalTwiceIsDeclined()
{
    StubSource source;
    TimeSeriesPanelConfig_t config;
    scope::TimeSeriesPanel plot(config, source);

    expect(plot.addBinding(numericField()), "the first add succeeds");
    expect(!plot.addBinding(numericField()),
           "adding the same signal again is declined rather than drawing it twice");
    expect(plot.getConfig().traces.size() == 1, "only one trace results");
}

void testTracesGetDistinctColours()
{
    StubSource source;
    TimeSeriesPanelConfig_t config;
    scope::TimeSeriesPanel plot(config, source);

    scope::BindingCandidate first = numericField();
    scope::BindingCandidate second = numericField();
    second.field_name = "oilPressurePsi";
    second.type_category = "float";

    plot.addBinding(first);
    plot.addBinding(second);

    expect(plot.getConfig().traces.size() == 2, "both signals are added");
    if (plot.getConfig().traces.size() == 2)
    {
        expect(plot.getConfig().traces[0].color.value() != plot.getConfig().traces[1].color.value(),
               "a second trace gets a different colour, or it would be invisible under the first");
    }
}

void testRemovingASignal()
{
    StubSource source;
    TimeSeriesPanelConfig_t config;
    scope::TimeSeriesPanel plot(config, source);
    plot.addBinding(numericField());

    expect(!plot.removeBinding(5), "removing a signal that is not there fails cleanly");
    expect(plot.removeBinding(0), "removing an existing signal succeeds");
    expect(plot.getConfig().traces.empty(), "the trace is gone from the config");
    expect(!source.released.empty(), "the binding was released on the source");
}

void runPlotTests()
{
    testAPlotAcceptsOnlyNumericFields();
    testAddingASignalBindsIt();
    testAListIsAcceptedOnItsElementType();
    testAListWithADeclaredLengthExpands();
    testEnumsAndBoolsBecomeLanes();
    testTheDisplayOverrideWinsBothWays();
    testAddingTheSameSignalTwiceIsDeclined();
    testTracesGetDistinctColours();
    testRemovingASignal();
}

}  // namespace panel_tests
