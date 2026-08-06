// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window and panel layer against a real widget tree: adding and removing
// panels, what a panel will and will not accept, and the dock-state fallback.
//
// A `gui` test, so it runs offscreen. It uses a stub DataSource rather than the
// live zenoh one -- everything here is about widget behaviour, and needing a
// bus would make it a `net` test that skips itself on a machine without one,
// which is exactly the coverage you lose first and miss most.

#include "scope/data_source.h"
#include "scope/panel_registry.h"
#include "scope/scope_window.h"
#include "scope/signal_browser.h"
#include "scope/time_base.h"

#include "time_series/time_series_panel.h"

#include <QApplication>
#include <QDockWidget>

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// A DataSource that binds anything and produces nothing, so panel behaviour can
// be tested without a bus. Binding succeeding is what the panel cares about;
// where the samples come from is LiveZenohSource's problem and is covered by
// the ring and evaluator tests.
class StubSource : public scope::DataSource
{
  public:
    scope::SourceCaps caps() const override
    {
        scope::SourceCaps caps;
        caps.live = true;
        caps.seekable = false;
        return caps;
    }

    std::vector<scope::TopicInfo> topics() const override { return available; }
    std::uint64_t topicsRevision() const override { return revision; }

    scope::SignalHandle bind(const scope::SignalKey& key,
                             std::shared_ptr<scope::SignalBuffer> into) override
    {
        bound.push_back(key);
        buffers.push_back(std::move(into));
        return next_handle++;
    }

    void release(scope::SignalHandle handle) override { released.push_back(handle); }

    double now() const override { return 100.0; }

    std::vector<scope::TopicInfo> available;
    std::uint64_t revision = 0;
    std::vector<scope::SignalKey> bound;
    std::vector<std::shared_ptr<scope::SignalBuffer>> buffers;
    std::vector<scope::SignalHandle> released;
    scope::SignalHandle next_handle = 1;
};

scope::BindingCandidate numericField()
{
    scope::BindingCandidate candidate;
    candidate.zenoh_key = "vehicle/engine/rpm";
    candidate.schema_name = "EngineRpm";
    candidate.field_name = "rpm";
    candidate.type_category = "uint";
    return candidate;
}

// ----------------------------------------------------------------- the registry

void testThePanelTableDrivesEverything()
{
    const std::vector<scope::PanelTypeInfo> types = scope::availablePanelTypes();
    expect(!types.empty(), "at least one panel type is registered");

    bool found_time_series = false;
    for (const scope::PanelTypeInfo& info : types)
    {
        if (info.type == scope::panel_type_t::time_series)
        {
            found_time_series = true;
            expect(info.name == "time_series", "the enum name comes from the table");
            expect(info.friendly_name == "Time Series", "the friendly name comes from the class");
        }
    }
    expect(found_time_series, "the time-series panel is in the registry");
}

void testDefaultConfigMatchesTheType()
{
    const scope::panel_config_variant_t config =
        scope::default_panel_config(scope::panel_type_t::time_series);
    expect(std::holds_alternative<TimeSeriesPanelConfig_t>(config),
           "a default config holds the alternative for its type");
    expect(scope::panelTypeOf(config) == scope::panel_type_t::time_series,
           "the type can be recovered from the config variant");

    const scope::panel_config_variant_t unknown =
        scope::default_panel_config(scope::panel_type_t::unknown);
    expect(std::holds_alternative<std::monostate>(unknown),
           "an unknown type produces monostate rather than some arbitrary panel");
    expect(scope::panelTypeOf(unknown) == scope::panel_type_t::unknown,
           "monostate reports back as unknown");
}

void testCreatingAnUnknownPanelReturnsNull()
{
    StubSource source;
    std::unique_ptr<scope::Panel> panel =
        scope::createPanel(scope::panel_config_variant_t{std::monostate{}}, source);
    expect(panel == nullptr,
           "an unknown panel type constructs nothing, rather than substituting another kind");
}

void testValidateClampsBeforeConstruction()
{
    // The panel must never see a config the loader would have clamped, or it
    // would report the unclamped values back when the workspace was saved.
    StubSource source;
    TimeSeriesPanelConfig_t config;
    config.window_seconds = 1e9;  // Far beyond the cap.
    config.y_min = 100.0;         // Inverted range.
    config.y_max = 0.0;

    std::unique_ptr<scope::Panel> panel = scope::createPanel(config, source);
    expect(panel != nullptr, "a panel with an out-of-range config still constructs");

    auto* plot = qobject_cast<scope::TimeSeriesPanel*>(panel.get());
    expect(plot != nullptr, "it is the right kind of panel");
    if (plot != nullptr)
    {
        expect(plot->getConfig().window_seconds <= 24.0 * 60.0 * 60.0,
               "an oversized window is clamped before the panel sees it");
        expect(plot->getConfig().y_min < plot->getConfig().y_max,
               "an inverted Y range is ordered before the panel sees it");
    }
}

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

    for (const char* category : {"int", "uint", "float", "bool"})
    {
        scope::BindingCandidate numeric = numericField();
        numeric.type_category = category;
        expect(plot.acceptsBinding(numeric),
               std::string("a '") + category + "' field is accepted");
    }

    for (const char* category : {"data", "list", "struct", "enum", "void", "other", ""})
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

    expect(!plot.removeSignal(5), "removing a signal that is not there fails cleanly");
    expect(plot.removeSignal(0), "removing an existing signal succeeds");
    expect(plot.getConfig().traces.empty(), "the trace is gone from the config");
    expect(!source.released.empty(), "the binding was released on the source");
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
    const std::vector<scope::TimeSeriesPanel::SignalStats> stats = plot.stats();

    expect(stats.size() == 1, "there is one signal to report on");
    if (!stats.empty())
    {
        expect(stats[0].label == "rpm", "the label is reported");
        expect(stats[0].bound, "a signal the source accepted reports as bound");
        expect(!stats[0].has_data, "a signal with no samples reports no data rather than zeros");
    }
}

// ---------------------------------------------------------------------- window

void testTheWindowAddsAndRemovesPanels()
{
    scope::ScopeWindow window;

    expect(window.panels().empty(), "a new window has no panels");

    const QString first = window.addPanel(scope::panel_type_t::time_series);
    expect(!first.isEmpty(), "adding a panel returns its id");
    expect(window.panels().size() == 1, "the panel is recorded");

    const QString second = window.addPanel(scope::panel_type_t::time_series);
    expect(second != first, "a second panel gets a different id");
    expect(window.panels().size() == 2, "both panels are recorded");

    expect(window.findPanel(first) != nullptr, "a panel can be found by id");
    expect(window.findPanel("nope") == nullptr, "an unknown id finds nothing");

    expect(window.removePanel(first), "removing a panel by id succeeds");
    expect(window.panels().size() == 1, "the panel is gone");
    expect(!window.removePanel(first), "removing it again fails cleanly");
}

void testEveryDockHasAnObjectName()
{
    // Load-bearing. restoreState() silently drops any dock it cannot name, so a
    // workspace would come back missing panels with nothing logged.
    scope::ScopeWindow window;
    const QString id = window.addPanel(scope::panel_type_t::time_series, "my_panel");

    const scope::ScopeWindow::PanelEntry* entry = window.findPanel(id);
    expect(entry != nullptr, "the panel exists");
    if (entry != nullptr)
    {
        expect(entry->dock->objectName() == "my_panel",
               "the dock's objectName is the panel id, which is what restoreState matches on");
    }
}

void testAnExplicitPanelIdIsHonoured()
{
    scope::ScopeWindow window;
    const QString id = window.addPanel(scope::panel_type_t::time_series, "engine");
    expect(id == "engine", "an explicit id is used verbatim, so a workspace can rely on it");
}

void testAddingAnUnknownPanelTypeFails()
{
    scope::ScopeWindow window;
    expect(window.addPanel(scope::panel_type_t::unknown).isEmpty(),
           "an unknown panel type adds nothing and says so");
    expect(window.panels().empty(), "no panel was created");
}

void testDockStateRoundTrips()
{
    scope::ScopeWindow window;
    window.addPanel(scope::panel_type_t::time_series, "a");
    window.addPanel(scope::panel_type_t::time_series, "b");

    const QByteArray state = window.dockState();
    expect(!state.isEmpty(), "a window with panels produces a dock state");
    expect(window.restoreDockState(state), "its own dock state restores");
}

void testGarbageDockStateIsRejectedNotFatal()
{
    // The blob is opaque and Qt-versioned, so a workspace written by another Qt
    // build will fail to restore. That has to be survivable: everything that
    // matters is in the readable YAML, and losing this costs an arrangement.
    scope::ScopeWindow window;
    window.addPanel(scope::panel_type_t::time_series, "a");

    expect(!window.restoreDockState(QByteArray()), "an empty dock state is refused");
    expect(!window.restoreDockState(QByteArray("not a qt dock state at all")),
           "a corrupt dock state is refused rather than crashing");
    expect(window.panels().size() == 1, "the panels are still there after a failed restore");
}

void testTheTimeBaseIsShared()
{
    scope::ScopeWindow window;
    window.addPanel(scope::panel_type_t::time_series, "a");
    window.addPanel(scope::panel_type_t::time_series, "b");

    scope::TimeBase& time_base = window.timeBase();
    time_base.setWindowSeconds(12.5);
    expect(time_base.windowSeconds() == 12.5, "the window length is settable");

    time_base.setMode(scope::TimeBase::Mode::Paused);
    const double frozen = time_base.viewEnd();
    expect(time_base.viewEnd() == frozen, "a paused view end does not move");

    time_base.setMode(scope::TimeBase::Mode::Live);
    expect(time_base.mode() == scope::TimeBase::Mode::Live, "it goes back to live");
}

void testTimeBaseClampsSillyValues()
{
    scope::ScopeWindow window;
    scope::TimeBase& time_base = window.timeBase();

    time_base.setWindowSeconds(-5.0);
    expect(time_base.windowSeconds() > 0.0, "a negative window is clamped, not accepted");

    time_base.setRenderRateHz(100000);
    expect(time_base.renderRateHz() <= 120,
           "an absurd render rate is clamped -- the dashboard once turned one into a 0 ms "
           "timer that fired on every pass of the event loop");

    time_base.setRenderRateHz(0);
    expect(time_base.renderRateHz() >= 1, "a zero render rate is clamped away from a 0 ms timer");
}

// ------------------------------------------------------------------- the drag

void testCandidateEncodingRoundTrips()
{
    const scope::BindingCandidate original = numericField();
    scope::BindingCandidate decoded;

    expect(scope::decodeCandidate(scope::encodeCandidate(original), decoded),
           "an encoded candidate decodes");
    expect(decoded.zenoh_key == original.zenoh_key, "the topic survives the drag");
    expect(decoded.schema_name == original.schema_name, "the schema survives the drag");
    expect(decoded.field_name == original.field_name, "the field survives the drag");
    expect(decoded.type_category == original.type_category, "the category survives the drag");
}

void testGarbageDropDataIsRejectedNotThrown()
{
    // Anything can be dropped on a widget, including a drag from a browser
    // window. This has to fail cleanly: an exception out of a Qt event handler
    // terminates the app, which is exactly how the editor's canvas learned it.
    scope::BindingCandidate decoded;

    expect(!scope::decodeCandidate(QByteArray(), decoded), "empty drop data is refused");
    expect(!scope::decodeCandidate(QByteArray("not json at all"), decoded),
           "non-JSON drop data is refused");
    expect(!scope::decodeCandidate(QByteArray("[1,2,3]"), decoded),
           "JSON that is not an object is refused");
    expect(!scope::decodeCandidate(QByteArray("{}"), decoded),
           "an object with no topic is refused");
    expect(!scope::decodeCandidate(QByteArray("{\"zenoh_key\": 42}"), decoded),
           "an object with a wrong-typed topic is refused");
}

}  // namespace

int main(int argc, char** argv)
{
    // Forced here as well as by the test harness, so a manual run behaves the
    // same as a ctest one.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    spdlog::set_level(spdlog::level::off);

    testThePanelTableDrivesEverything();
    testDefaultConfigMatchesTheType();
    testCreatingAnUnknownPanelReturnsNull();
    testValidateClampsBeforeConstruction();

    testAPlotAcceptsOnlyNumericFields();
    testAddingASignalBindsIt();
    testAddingTheSameSignalTwiceIsDeclined();
    testTracesGetDistinctColours();
    testRemovingASignal();
    testStatsReportUnboundSignals();

    testTheWindowAddsAndRemovesPanels();
    testEveryDockHasAnObjectName();
    testAnExplicitPanelIdIsHonoured();
    testAddingAnUnknownPanelTypeFails();
    testDockStateRoundTrips();
    testGarbageDockStateIsRejectedNotFatal();
    testTheTimeBaseIsShared();
    testTimeBaseClampsSillyValues();

    testCandidateEncodingRoundTrips();
    testGarbageDropDataIsRejectedNotThrown();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
