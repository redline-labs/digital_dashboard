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
#include "scope/overview_strip.h"
#include "scope/panel_registry.h"
#include "scope/scope_window.h"
#include "scope/signal_browser.h"
#include "scope/time_base.h"

#include "time_series/time_series_panel.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QMouseEvent>
#include <QPixmap>
#include <QToolButton>
#include <QWheelEvent>

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <filesystem>
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

// A stub that reports itself as a recording, for the parts of the window that
// render from caps() rather than from what the source actually contains.
class SeekableStub : public StubSource
{
  public:
    scope::SourceCaps caps() const override
    {
        scope::SourceCaps caps;
        caps.live = false;
        caps.seekable = true;
        caps.t_begin = 0.0;
        caps.t_end = 120.0;
        return caps;
    }
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

// ------------------------------------------------------------------ gestures
//
// These need a real widget tree -- a gesture converts pixels against what the
// panel actually drew -- which is why they live here rather than in
// scope_test_time_base. Synthesised events are delivered fine offscreen; it is
// only QDrag::exec() that cannot run there.

namespace
{

// A panel big enough to have a usable plot rect. Below the gutters plotRect()
// returns its 1x1 degenerate guard and every gesture is correctly a no-op --
// which would make these tests pass while proving nothing.
// paintEvent is what fills drawn_begin_/drawn_end_, and every gesture maps
// against those rather than against the time base -- deliberately, so a click
// lands on the instant the user can see. The consequence for a test is that
// moving the view and then sending a gesture without a repaint in between
// converts against the PREVIOUS window. Rendering into a pixmap is the only way
// to force a paint with no compositor.
void forcePaint(QWidget* panel)
{
    QPixmap scratch(panel->size());
    panel->render(&scratch);
}

scope::TimeSeriesPanel* readyPanel(scope::ScopeWindow& window, const QString& id)
{
    window.addPanel(scope::panel_type_t::time_series, id);
    auto* panel = static_cast<scope::TimeSeriesPanel*>(window.findPanel(id)->panel);
    panel->resize(600, 400);
    forcePaint(panel);
    return panel;
}

// The plot rect's horizontal extent, from the gutter constants. Duplicated from
// the panel rather than exposed, because a test that read the panel's own
// arithmetic back would agree with it however wrong it was.
constexpr double kPlotLeft = 56.0;
constexpr double kPlotRight = 600.0 - 12.0;

void wheel(QWidget* target, QPointF at, int delta, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QWheelEvent event(at, target->mapToGlobal(at), QPoint(), QPoint(0, delta), Qt::NoButton, mods,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(target, &event);
}

void mouse(QWidget* target, QEvent::Type type, QPointF at, Qt::MouseButton button,
           Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent event(type, at, target->mapToGlobal(at), button, buttons, mods);
    QCoreApplication::sendEvent(target, &event);
}

}  // namespace

void testWheelZoomHoldsTheInstantUnderThePointer()
{
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* panel = readyPanel(window, "a");

    scope::TimeBase& time_base = window.timeBase();
    time_base.setRetentionSeconds(100000.0);
    time_base.setMode(scope::TimeBase::Mode::Paused);
    time_base.setView(-500.0, -440.0);

    forcePaint(panel);

    const double before = time_base.windowSeconds();

    // Middle of the plot, so the pivot is nowhere near a clamp.
    const QPointF at(300.0, 200.0);
    const double under_pointer =
        time_base.viewBegin() + (at.x() - kPlotLeft) / (kPlotRight - kPlotLeft) * before;

    wheel(panel, at, 240);

    expect(time_base.windowSeconds() < before, "a wheel forward zooms in");

    // The property that matters. Everything else about wheel zoom is chrome.
    expect(under_pointer >= time_base.viewBegin() - 1e-6 &&
               under_pointer <= time_base.viewEnd() + 1e-6,
           "the instant under the pointer is still in the window after a zoom");
}

void testAZoomOnOnePanelMovesTheOther()
{
    // The whole requirement: a plot is a view onto ONE window. If this fails the
    // panels have quietly grown independent axes and the shared cursor lines up
    // with nothing.
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* a = readyPanel(window, "a");
    scope::TimeSeriesPanel* b = readyPanel(window, "b");
    (void)b;

    scope::TimeBase& time_base = window.timeBase();
    time_base.setRetentionSeconds(100000.0);
    time_base.setMode(scope::TimeBase::Mode::Paused);
    time_base.setView(-500.0, -440.0);
    forcePaint(a);

    const double before = time_base.windowSeconds();
    wheel(a, QPointF(300.0, 200.0), 240);

    expect(time_base.windowSeconds() < before,
           "zooming panel a changed the window both panels draw");
}

void testHoveringDoesNotPan()
{
    // Regression guard. mouseMoveEvent handles both the shared cursor and the
    // pan, and an early version of the drag branch swallowed the hover.
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* panel = readyPanel(window, "a");

    scope::TimeBase& time_base = window.timeBase();
    time_base.setRetentionSeconds(100000.0);
    time_base.setMode(scope::TimeBase::Mode::Paused);
    time_base.setView(-500.0, -440.0);
    forcePaint(panel);

    const double begin = time_base.viewBegin();

    mouse(panel, QEvent::MouseMove, QPointF(300.0, 200.0), Qt::NoButton, Qt::NoButton);

    expect(time_base.viewBegin() == begin, "hovering leaves the window alone");
    expect(time_base.cursor().has_value(), "and does set the shared cursor");
}

void testAClickThatDoesNotTravelIsNotAPan()
{
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* panel = readyPanel(window, "a");

    scope::TimeBase& time_base = window.timeBase();
    time_base.setRetentionSeconds(100000.0);
    time_base.setMode(scope::TimeBase::Mode::Paused);
    time_base.setView(-500.0, -440.0);
    forcePaint(panel);

    const double begin = time_base.viewBegin();

    mouse(panel, QEvent::MouseButtonPress, QPointF(300.0, 200.0), Qt::LeftButton, Qt::LeftButton);
    mouse(panel, QEvent::MouseMove, QPointF(301.0, 200.0), Qt::NoButton, Qt::LeftButton);
    mouse(panel, QEvent::MouseButtonRelease, QPointF(301.0, 200.0), Qt::LeftButton, Qt::NoButton);

    expect(time_base.viewBegin() == begin,
           "a press and release that never travels leaves the window alone");
}

void testDraggingPansTheSharedWindow()
{
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* panel = readyPanel(window, "a");

    scope::TimeBase& time_base = window.timeBase();
    time_base.setRetentionSeconds(100000.0);
    time_base.setMode(scope::TimeBase::Mode::Paused);
    time_base.setView(-500.0, -440.0);
    forcePaint(panel);

    const double begin = time_base.viewBegin();
    const double span = time_base.windowSeconds();

    mouse(panel, QEvent::MouseButtonPress, QPointF(300.0, 200.0), Qt::LeftButton, Qt::LeftButton);
    mouse(panel, QEvent::MouseMove, QPointF(200.0, 200.0), Qt::NoButton, Qt::LeftButton);
    mouse(panel, QEvent::MouseButtonRelease, QPointF(200.0, 200.0), Qt::LeftButton, Qt::NoButton);

    // Dragging LEFT moves the window forward in time: the content follows the
    // hand, which is the direction every map and every document uses.
    expect(time_base.viewBegin() > begin, "dragging left moves the window forwards");
    expect(std::abs(time_base.windowSeconds() - span) < 1e-9, "and does not change the zoom");
}

void testShiftDragZoomsToTheBand()
{
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* panel = readyPanel(window, "a");

    scope::TimeBase& time_base = window.timeBase();
    time_base.setRetentionSeconds(100000.0);
    time_base.setMode(scope::TimeBase::Mode::Paused);
    time_base.setView(-500.0, -440.0);
    forcePaint(panel);

    const double span = time_base.windowSeconds();

    mouse(panel, QEvent::MouseButtonPress, QPointF(200.0, 200.0), Qt::LeftButton, Qt::LeftButton,
          Qt::ShiftModifier);
    mouse(panel, QEvent::MouseMove, QPointF(300.0, 200.0), Qt::NoButton, Qt::LeftButton,
          Qt::ShiftModifier);
    mouse(panel, QEvent::MouseButtonRelease, QPointF(300.0, 200.0), Qt::LeftButton, Qt::NoButton,
          Qt::ShiftModifier);

    expect(time_base.windowSeconds() < span, "shift-dragging a band zooms into it");
}

void testShiftWheelTurnsOffAutoscale()
{
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* panel = readyPanel(window, "a");

    expect(panel->getConfig().autoscale_y, "autoscale starts on");

    wheel(panel, QPointF(300.0, 200.0), 240, Qt::ShiftModifier);
    expect(!panel->getConfig().autoscale_y, "a shift-wheel takes manual control of the Y axis");

    // The one-gesture way back. Without it a stray scroll strands the panel on a
    // range the user has to go into a config dialog to undo.
    mouse(panel, QEvent::MouseButtonDblClick, QPointF(300.0, 200.0), Qt::LeftButton,
          Qt::LeftButton);
    expect(panel->getConfig().autoscale_y, "a double-click gives it back");
}

void testDoubleClickResumesFollowing()
{
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* panel = readyPanel(window, "a");

    scope::TimeBase& time_base = window.timeBase();
    time_base.setMode(scope::TimeBase::Mode::Paused);
    expect(!time_base.following(), "paused, so not following");

    mouse(panel, QEvent::MouseButtonDblClick, QPointF(300.0, 200.0), Qt::LeftButton,
          Qt::LeftButton);

    expect(time_base.following(), "double-clicking a live panel catches back up with the bus");
}

// ------------------------------------------------------------------- toolbar

void testTheToolbarReusesTheMenusActions()
{
    // The discipline that keeps a toolbar honest: ONE QAction per thing, living
    // in both places. Two copies would have two objectNames, two handlers and
    // two enabled-states, and the first guard added to one would silently not
    // apply to the other.
    scope::ScopeWindow window;

    for (const char* name : {"action_add_time_series", "action_open", "action_save",
                             "action_open_recording", "action_view_browser", "action_zoom_in",
                             "action_zoom_out", "action_zoom_fit"})
    {
        expect(window.findChildren<QAction*>(name).size() == 1,
               std::string("exactly one QAction named ") + name);
    }
}

void testTheToolbarOffersEveryPanelType()
{
    // Generated from the panel table, so a new panel type reaches the toolbar
    // with no UI change. If this ever needs editing to add a panel, the
    // generation has been undone.
    scope::ScopeWindow window;
    for (const scope::PanelTypeInfo& info : scope::availablePanelTypes())
    {
        const QString name =
            QStringLiteral("action_add_%1")
                .arg(QString::fromUtf8(info.name.data(), static_cast<qsizetype>(info.name.size())));
        expect(window.findChild<QAction*>(name) != nullptr,
               std::string("the toolbar can add a ") + std::string(info.friendly_name));
        expect(!info.toolbar_glyph.empty(),
               std::string("and has a glyph for it: ") + std::string(info.name));
    }
}

void testTheModeButtonsFollowTheSourceNotTheClick()
{
    scope::ScopeWindow window;

    auto* live = window.findChild<QToolButton*>("mode_live");
    auto* review = window.findChild<QToolButton*>("mode_review");
    expect(live != nullptr && review != nullptr, "the mode control exists");
    if (live == nullptr || review == nullptr)
    {
        return;
    }

    expect(live->isChecked(), "a fresh window is live");
    expect(!review->isChecked(), "and not reviewing");

    // Swapped WITHOUT going near the buttons, which is what --bag at startup and
    // the agent interface both do. A pair of buttons tracking only their own
    // clicks would still be claiming Live here.
    auto seekable = std::make_unique<SeekableStub>();
    window.setSource(std::move(seekable));

    expect(!live->isChecked(), "a source swapped from elsewhere unchecks Live");
    expect(review->isChecked(), "and checks Review");
}

void testPauseFollowsAPanRatherThanOnlyItsOwnClicks()
{
    // A pan turns following off without touching the button. Left to its own
    // toggled() the button sits there saying "Pause" over a plot that has
    // stopped scrolling, which is the most confusing state in the window.
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* panel = readyPanel(window, "a");
    (void)panel;

    auto* pause = window.findChild<QToolButton*>("transport_pause");
    expect(pause != nullptr, "the pause button exists");
    if (pause == nullptr)
    {
        return;
    }
    expect(!pause->isChecked(), "not paused to begin with");

    window.timeBase().setRetentionSeconds(1000.0);
    window.timeBase().panBy(-50.0);

    expect(pause->isChecked(), "panning away from the live edge shows as paused");
}

void testNavigationActionsMoveTheSharedWindow()
{
    scope::ScopeWindow window;
    scope::TimeBase& time_base = window.timeBase();
    time_base.setRetentionSeconds(100000.0);
    time_base.setMode(scope::TimeBase::Mode::Paused);
    time_base.setView(-500.0, -440.0);

    const double span = time_base.windowSeconds();

    window.findChild<QAction*>("action_zoom_in")->trigger();
    expect(time_base.windowSeconds() < span, "the zoom-in action narrows the window");

    window.findChild<QAction*>("action_zoom_out")->trigger();
    expect(std::abs(time_base.windowSeconds() - span) < 1e-9,
           "and zoom-out is its exact inverse");

    const double begin = time_base.viewBegin();
    window.findChild<QAction*>("action_pan_back")->trigger();
    expect(time_base.viewBegin() < begin, "the pan-back action moves the window earlier");

    window.findChild<QAction*>("action_zoom_fit")->trigger();
    expect(time_base.windowSeconds() > span, "fit widens to everything available");
}

// ------------------------------------------------------------ overview strip

namespace
{

// The strip is a dumb painter: ScopeWindow pushes numbers in and connects to
// what comes out. That is what makes it testable with four setters and a
// synthesised drag, with no source and no bus anywhere.
scope::OverviewStrip* readyStrip()
{
    auto* strip = new scope::OverviewStrip();
    strip->resize(1000, 48);
    strip->setExtent(0.0, 100.0);
    strip->setView(40.0, 60.0);
    return strip;
}

}  // namespace

void testTheStripHitTestsEdgesBeforeTheBody()
{
    // The edges are the ZOOM handles and the body is the PAN handle. Testing the
    // body first makes the edges unreachable on any view wider than the grab
    // margin -- which is almost all of them -- so a user aiming at an edge pans
    // instead, silently and in the wrong dimension.
    std::unique_ptr<scope::OverviewStrip> strip(readyStrip());

    double begin = 0.0;
    double end = 0.0;
    QObject::connect(strip.get(), &scope::OverviewStrip::viewRequested,
                     [&](double b, double e) {
                         begin = b;
                         end = e;
                     });

    // The view is [40, 60] over [0, 100] on a 1000px widget, so its edges are at
    // x = 400 and x = 600. Grab the left edge and drag it to x = 300.
    mouse(strip.get(), QEvent::MouseButtonPress, QPointF(400.0, 20.0), Qt::LeftButton,
          Qt::LeftButton);
    mouse(strip.get(), QEvent::MouseMove, QPointF(300.0, 20.0), Qt::NoButton, Qt::LeftButton);
    mouse(strip.get(), QEvent::MouseButtonRelease, QPointF(300.0, 20.0), Qt::LeftButton,
          Qt::NoButton);

    expect(std::abs(begin - 30.0) < 0.5, "dragging the left edge moves only that edge");
    expect(std::abs(end - 60.0) < 0.5, "and leaves the right one alone -- that is a zoom");
}

void testTheStripBodyDragPansWithoutZooming()
{
    std::unique_ptr<scope::OverviewStrip> strip(readyStrip());

    double begin = 0.0;
    double end = 0.0;
    QObject::connect(strip.get(), &scope::OverviewStrip::viewRequested,
                     [&](double b, double e) {
                         begin = b;
                         end = e;
                     });

    // Grab the middle of the region and drag right by 100px = 10s.
    mouse(strip.get(), QEvent::MouseButtonPress, QPointF(500.0, 20.0), Qt::LeftButton,
          Qt::LeftButton);
    mouse(strip.get(), QEvent::MouseMove, QPointF(600.0, 20.0), Qt::NoButton, Qt::LeftButton);

    expect(std::abs((end - begin) - 20.0) < 0.5, "a body drag keeps the span");
    expect(std::abs(begin - 50.0) < 0.5, "and moves it by the drag distance");
}

void testTheStripKeepsTheGrabOffset()
{
    // Held so the region moves WITH the pointer rather than centring on it.
    // Centring makes the window jump on the first pixel of every drag, which
    // reads as the strip snatching the view away from where it was.
    std::unique_ptr<scope::OverviewStrip> strip(readyStrip());

    double begin = -1.0;
    QObject::connect(strip.get(), &scope::OverviewStrip::viewRequested,
                     [&](double b, double) { begin = b; });

    // Press near the LEFT of the region, then move by one pixel.
    mouse(strip.get(), QEvent::MouseButtonPress, QPointF(420.0, 20.0), Qt::LeftButton,
          Qt::LeftButton);
    mouse(strip.get(), QEvent::MouseMove, QPointF(421.0, 20.0), Qt::NoButton, Qt::LeftButton);

    expect(std::abs(begin - 40.1) < 0.2,
           "a one-pixel drag moves the window one pixel, not to the pointer");
}

void testClickingOutsideTheRegionCentresTheView()
{
    std::unique_ptr<scope::OverviewStrip> strip(readyStrip());

    double begin = 0.0;
    double end = 0.0;
    QObject::connect(strip.get(), &scope::OverviewStrip::viewRequested,
                     [&](double b, double e) {
                         begin = b;
                         end = e;
                     });

    // Jumping to a place you pointed at is the one thing the QSlider this
    // replaced did well, and losing it would make the strip worse at the coarse
    // case it is best at.
    mouse(strip.get(), QEvent::MouseButtonPress, QPointF(800.0, 20.0), Qt::LeftButton,
          Qt::LeftButton);

    expect(std::abs(((begin + end) / 2.0) - 80.0) < 0.5, "a click outside centres the view on it");
    expect(std::abs((end - begin) - 20.0) < 0.5, "keeping the span");
}

void testTheStripBracketsItsDragForCoalescing()
{
    // The window uses this to hold TimeBase::setInteracting() for the drag, so
    // the seeks a drag generates coalesce to one per frame. Without the pair,
    // every mouse-move refills a whole retention window per bound signal.
    std::unique_ptr<scope::OverviewStrip> strip(readyStrip());

    std::vector<bool> interactions;
    QObject::connect(strip.get(), &scope::OverviewStrip::interactionChanged,
                     [&](bool active) { interactions.push_back(active); });

    mouse(strip.get(), QEvent::MouseButtonPress, QPointF(500.0, 20.0), Qt::LeftButton,
          Qt::LeftButton);
    mouse(strip.get(), QEvent::MouseMove, QPointF(520.0, 20.0), Qt::NoButton, Qt::LeftButton);
    mouse(strip.get(), QEvent::MouseButtonRelease, QPointF(520.0, 20.0), Qt::LeftButton,
          Qt::NoButton);

    expect(interactions.size() == 2 && interactions[0] && !interactions[1],
           "a drag brackets itself with exactly one true and one false");
}

void testTheStripReplacedTheScrubber()
{
    // The one objectName that could not survive. Its replacement is not a
    // QSlider, so keeping the name would make an agent that clicks it and then
    // sets a value fail in a way that looks like a broken app rather than a
    // renamed widget.
    scope::ScopeWindow window;
    expect(window.findChild<QWidget*>("transport_scrubber") == nullptr,
           "transport_scrubber is gone, not quietly re-pointed at something else");
    expect(window.findChild<scope::OverviewStrip*>("overview_strip") != nullptr,
           "and the overview strip is there instead");
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

// --------------------------------------------------------- swapping sources

// A panel moved onto a different source must RELEASE against the old one first.
//
// A handle means nothing to a source that did not issue it, and the window
// destroys the old source only after the panels have rebound -- which is what
// makes the release legal and is the entire ordering rule. Repointing first
// leaves every subscription on the old source alive, and for the live one that
// means zenoh callbacks still decoding samples into buffers nobody will drain.
//
// Mutation-check: move `source_ = &source;` above `releaseAll();` in
// TimeSeriesPanel::rebindTo() and this fails -- the release lands on the new
// source, which has never heard of the handle.
void testRebindingReleasesAgainstTheOldSource()
{
    StubSource first;
    StubSource second;

    TimeSeriesPanelConfig_t config;
    scope::TimeSeriesPanel plot(config, first);
    plot.addBinding(numericField());

    expect(first.bound.size() == 1, "the signal is bound on the first source");
    expect(second.bound.empty(), "and not on the second");

    const scope::SignalHandle issued = first.bound.empty() ? scope::kInvalidSignal : 1;

    plot.rebindTo(second);

    expect(first.released.size() == 1,
           "the binding was released against the source that ISSUED it (" +
               std::to_string(first.released.size()) + ")");
    expect(!first.released.empty() && first.released[0] == issued,
           "and it is the handle that source handed out");
    expect(second.released.empty(),
           "nothing was released against the new source, which never issued anything");

    expect(second.bound.size() == 1, "and the trace is rebound on the new source");
    if (!second.bound.empty())
    {
        expect(second.bound[0].zenoh_key == "vehicle/engine/rpm",
               "on the same topic -- the workspace is untouched by a source swap");
    }
    expect(plot.getConfig().traces.size() == 1, "the config still describes one trace");

    plot.rebindTo(second);
    expect(second.bound.size() == 1, "rebinding to the source it is already on does nothing");
}

// ------------------------------------------------------------- retention

// `history_seconds` round-trips through the WINDOW, not just through the YAML.
//
// The codec always carried this field and scope_test_workspace always asserted
// it survived a save and a load -- while toWorkspace() never wrote it and
// loadWorkspace() never read it, so retention came from a constant in
// time_series_panel.cpp and the setting did nothing at all. A field that
// round-trips perfectly and is ignored at both ends is the worst kind of dead
// config: every test passes and the knob is not connected to anything.
//
// Mutation-check: drop either the `workspace.history_seconds = ...` in
// toWorkspace() or the setHistorySeconds() call in loadWorkspace(), and one of
// these fails.
void testHistorySecondsReachesThePanels()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "redline_scope_history_test.yaml";
    std::filesystem::remove(path);

    {
        scope::ScopeWindow window;
        window.setHistorySeconds(45.0);
        window.addPanel(scope::panel_type_t::time_series, "plot");

        const scope::ScopeWindow::PanelEntry* entry = window.findPanel("plot");
        expect(entry != nullptr, "the panel exists");
        if (entry != nullptr)
        {
            auto* plot = qobject_cast<scope::TimeSeriesPanel*>(entry->panel);
            expect(plot != nullptr && plot->historySeconds() == 45.0,
                   "a panel is built with the window's retention, not a constant");
        }

        expect(window.toWorkspace().history_seconds == 45.0,
               "and toWorkspace() writes it out");
        expect(window.saveWorkspace(QString::fromStdString(path.string())),
               "the workspace saves");
    }

    {
        scope::ScopeWindow window;
        expect(window.loadWorkspace(QString::fromStdString(path.string())),
               "the workspace loads back");
        expect(window.historySeconds() == 45.0, "loadWorkspace() reads the retention");

        const scope::ScopeWindow::PanelEntry* entry = window.findPanel("plot");
        expect(entry != nullptr, "the panel came back");
        if (entry != nullptr)
        {
            auto* plot = qobject_cast<scope::TimeSeriesPanel*>(entry->panel);
            expect(plot != nullptr && plot->historySeconds() == 45.0,
                   "and its panels are built with it -- retention applies BEFORE binding, "
                   "because a buffer cannot grow a past it never recorded");
        }
    }

    std::filesystem::remove(path);
}

void testRetentionIsClampedNotRefused()
{
    scope::ScopeWindow window;

    window.setHistorySeconds(-1.0);
    expect(window.historySeconds() >= 1.0, "a negative retention is clamped, not accepted");

    window.setHistorySeconds(1e9);
    expect(window.historySeconds() <= 24.0 * 60.0 * 60.0, "an absurd retention is clamped");
}

// ------------------------------------------------------------- dirty state

void testDirtyTracking()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "redline_scope_dirty_test.yaml";
    std::filesystem::remove(path);

    scope::ScopeWindow window;
    expect(!window.isDirty(),
           "an empty window is not dirty -- prompting on the way out of one would train the "
           "user to dismiss the prompt that matters");

    window.addPanel(scope::panel_type_t::time_series, "plot");
    expect(window.isDirty(), "adding a panel makes the workspace dirty");
    expect(window.windowTitle().endsWith(" *"), "and the title says so");

    expect(window.saveWorkspace(QString::fromStdString(path.string())), "it saves");
    expect(!window.isDirty(), "saving makes it clean again");
    expect(!window.windowTitle().endsWith(" *"), "and the marker goes away");

    scope::ScopeWindow::PanelEntry* entry = window.findPanel("plot");
    if (entry != nullptr)
    {
        auto* plot = qobject_cast<scope::TimeSeriesPanel*>(entry->panel);
        expect(plot != nullptr && plot->addBinding(numericField()), "a signal is added");
        expect(window.isDirty(),
               "a panel's own config change reaches the window -- the panel says so rather "
               "than the window guessing from the several routes in");
    }

    window.removePanel("plot");
    expect(window.isDirty(), "removing a panel makes it dirty");

    expect(window.loadWorkspace(QString::fromStdString(path.string())),
           "the saved workspace loads");
    expect(!window.isDirty(), "a freshly loaded workspace is clean");

    std::filesystem::remove(path);
}

void testHeadlessNeverBlocksOnADialog()
{
    // THE reason setHeadless() exists. Under --mcp there is nobody to dismiss a
    // modal dialog, so one raised here does not fail -- it hangs the process,
    // with no log line and no error, which is the hardest kind of bug to find
    // from the other side of a socket.
    scope::ScopeWindow window;
    window.setHeadless(true);
    window.addPanel(scope::panel_type_t::time_series, "plot");
    expect(window.isDirty(), "there are unsaved changes to prompt about");

    // Returns rather than hanging: that is the assertion. A test that reached a
    // QMessageBox here would time out instead of failing.
    expect(window.confirmDiscardChanges("closing"),
           "a headless window discards with a warning rather than raising a prompt");

    expect(!window.saveWorkspaceDialog(),
           "and the Save dialog refuses headlessly instead of opening a file picker");
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

    testTheStripHitTestsEdgesBeforeTheBody();
    testTheStripBodyDragPansWithoutZooming();
    testTheStripKeepsTheGrabOffset();
    testClickingOutsideTheRegionCentresTheView();
    testTheStripBracketsItsDragForCoalescing();
    testTheStripReplacedTheScrubber();

    testTheToolbarReusesTheMenusActions();
    testTheToolbarOffersEveryPanelType();
    testTheModeButtonsFollowTheSourceNotTheClick();
    testPauseFollowsAPanRatherThanOnlyItsOwnClicks();
    testNavigationActionsMoveTheSharedWindow();

    testWheelZoomHoldsTheInstantUnderThePointer();
    testAZoomOnOnePanelMovesTheOther();
    testHoveringDoesNotPan();
    testAClickThatDoesNotTravelIsNotAPan();
    testDraggingPansTheSharedWindow();
    testShiftDragZoomsToTheBand();
    testShiftWheelTurnsOffAutoscale();
    testDoubleClickResumesFollowing();

    testRebindingReleasesAgainstTheOldSource();
    testHistorySecondsReachesThePanels();
    testRetentionIsClampedNotRefused();
    testDirtyTracking();
    testHeadlessNeverBlocksOnADialog();

    testCandidateEncodingRoundTrips();
    testGarbageDropDataIsRejectedNotThrown();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
