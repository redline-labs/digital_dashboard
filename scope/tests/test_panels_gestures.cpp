// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "test_panels_common.h"

// Pixels to time: wheel/drag/band gestures against what the panel
// actually drew, and the table's column geometry worked out from the layout
// rules rather than read back.

namespace panel_tests
{

// ------------------------------------------------------------------ gestures
//
// These need a real widget tree -- a gesture converts pixels against what the
// panel actually drew -- which is why they live here rather than in
// scope_test_time_base. Synthesised events are delivered fine offscreen; it is
// only QDrag::exec() that cannot run there.

namespace
{


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


// ---------------------------------------------- the table's column geometry
//
// The expected numbers below are worked out from the layout RULES rather than
// read back from the panel. A test that asked the panel where its dividers are
// and then checked it dragged them there would agree with the arithmetic however
// wrong it was -- which is the same reasoning kPlotLeft/kPlotRight above are
// duplicated for, and it is not hypothetical: writing this out by hand is what
// found the value column being measured from the units column's RIGHT edge, so
// the two overlapped by 42 px whenever units were shown.
//
// Panel 600 px wide, everything automatic:
//   pad 8, gap 10, age 64, units 52, value share 0.40 clamped to [96, 240]
//   age_left   = 600 - 8 - 64                     = 528
//   units_left = 528 - 10 - 52                    = 466
//   value_right= 466 - 10                         = 456
//   usable     = 456 - 8 - 10                     = 438
//   value_w    = clamp(438 * 0.40, 96, 240)       = 175.2
//   value_left = 456 - 175.2                      = 280.8
// and a divider is drawn (and grabbed) in the middle of the gap before its
// column, so five pixels left of each of those.
constexpr double kTableWidth = 600.0;
constexpr double kAgeLeft = 528.0;
constexpr double kUnitsLeft = 466.0;
constexpr double kValueRight = 456.0;
constexpr double kValueLeft = 280.8;
constexpr double kDividerOffset = 5.0;

scope::TablePanel* sizedTable(scope::TablePanel& table)
{
    table.resize(static_cast<int>(kTableWidth), 400);
    forcePaint(&table);
    return &table;
}

}  // namespace

// The layout rules, before any gesture touches them.
void testTableColumnsSizeThemselvesUntilTheyAreSet()
{
    StubSource source;
    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());

    scope::TablePanel table(cfg, source);
    sizedTable(table);

    const scope::TablePanel::Columns automatic = table.columns();
    expect(std::abs(automatic.age_left - kAgeLeft) < 0.01, "the age column sits at the right edge");
    expect(std::abs(automatic.units_left - kUnitsLeft) < 0.01, "the units column sits beside it");
    expect(std::abs(automatic.value_left - kValueLeft) < 0.01,
           "and an untouched value column takes its share of the panel");
    expect(std::abs((automatic.value_left + automatic.value_width) - kValueRight) < 0.01,
           "the value column ends where the units column's gap begins -- NOT overlapping it");
    expect(automatic.name_width > 0.0 && automatic.name_left == 8.0,
           "the name column absorbs the rest");

    // An explicit width pins it, and the name column gives up the difference.
    TablePanelConfig_t explicit_width = cfg;
    explicit_width.value_width = 300.0;
    table.applyConfig(explicit_width);

    const scope::TablePanel::Columns pinned = table.columns();
    expect(std::abs(pinned.value_width - 300.0) < 0.01, "a set width is used exactly");
    expect(std::abs(pinned.name_width - (automatic.name_width - (300.0 - automatic.value_width))) <
               0.01,
           "and the name column shrinks by precisely that much");

    // A width that would leave no room for the names is refused: rows of numbers
    // with nothing saying what they are numbers OF is worse than a value that
    // elides, because an elided number still reads as a number.
    TablePanelConfig_t greedy = cfg;
    greedy.value_width = 380.0;
    table.applyConfig(greedy);
    expect(table.columns().name_width >= 40.0,
           "the name column keeps its minimum even against a width that asked for everything");

    // Hiding a column takes its space, and its divider, out of the layout.
    TablePanelConfig_t no_age = cfg;
    no_age.show_age = false;
    table.applyConfig(no_age);
    expect(table.columns().age_width == 0.0, "a hidden column has no width");
    expect(std::abs((table.columns().units_left + table.columns().units_width) -
                    (kTableWidth - 8.0)) < 0.01,
           "and the columns beside it move out to the edge it vacated");
}

// THE GESTURE: grab the line and pull. The three sized columns are packed
// against the right edge, so a divider moves its own column's left edge and the
// name column absorbs the difference.
void testDraggingADividerResizesThatColumn()
{
    StubSource source;
    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());

    scope::TablePanel table(cfg, source);
    sizedTable(table);

    int config_changes = 0;
    QObject::connect(&table, &scope::Panel::configChanged, &table,
                     [&config_changes]() { ++config_changes; });

    const double before = table.columns().value_width;
    const double grab = kValueLeft - kDividerOffset;

    // Seventy-five pixels to the LEFT, which makes the value column wider.
    mouse(&table, QEvent::MouseButtonPress, QPointF(grab, 200.0), Qt::LeftButton, Qt::LeftButton);
    mouse(&table, QEvent::MouseMove, QPointF(grab - 75.0, 200.0), Qt::NoButton, Qt::LeftButton);
    mouse(&table, QEvent::MouseButtonRelease, QPointF(grab - 75.0, 200.0), Qt::LeftButton,
          Qt::NoButton);

    expect(std::abs(table.columns().value_width - (before + 75.0)) < 0.01,
           "dragging the divider left widens the value column by exactly the distance dragged");
    expect(std::abs(table.getConfig().value_width - (before + 75.0)) < 0.01,
           "and the width lands in the CONFIG, so a workspace keeps it");
    expect(std::abs((table.columns().value_left + table.columns().value_width) - kValueRight) <
               0.01,
           "the column's right edge does not move -- only the edge that was dragged");

    expect(config_changes == 1,
           "one configChanged for the whole gesture, not one per mouse-move: the workspace is "
           "dirtied once and a listener that does real work cannot stutter the drag");

    // The other two dividers resize their own columns and nothing else.
    const double age_before = table.columns().age_width;
    mouse(&table, QEvent::MouseButtonPress, QPointF(kAgeLeft - kDividerOffset, 200.0),
          Qt::LeftButton, Qt::LeftButton);
    mouse(&table, QEvent::MouseMove, QPointF(kAgeLeft - kDividerOffset - 20.0, 200.0),
          Qt::NoButton, Qt::LeftButton);
    mouse(&table, QEvent::MouseButtonRelease, QPointF(kAgeLeft - kDividerOffset - 20.0, 200.0),
          Qt::LeftButton, Qt::NoButton);

    expect(std::abs(table.columns().age_width - (age_before + 20.0)) < 0.01,
           "the age divider widens the age column");
    expect(table.getConfig().units_width < 0.0,
           "and leaves the units column on automatic, which it never touched");
}

// A drag has to stop somewhere, and where it stops has to be what gets SAVED --
// otherwise the workspace describes a layout nobody has seen and reloading it
// appears to move the columns by itself.
void testADragCannotSaveAWidthItCannotShow()
{
    StubSource source;
    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());

    scope::TablePanel table(cfg, source);
    sizedTable(table);

    // Far past the left edge of the panel.
    const double grab = kValueLeft - kDividerOffset;
    mouse(&table, QEvent::MouseButtonPress, QPointF(grab, 200.0), Qt::LeftButton, Qt::LeftButton);
    mouse(&table, QEvent::MouseMove, QPointF(-400.0, 200.0), Qt::NoButton, Qt::LeftButton);
    mouse(&table, QEvent::MouseButtonRelease, QPointF(-400.0, 200.0), Qt::LeftButton,
          Qt::NoButton);

    expect(table.columns().name_width >= 40.0, "the name column survives a runaway drag");
    expect(std::abs(table.getConfig().value_width - table.columns().value_width) < 0.01,
           "and what was saved is exactly what is drawn");
}

// The way back from a bad drag. Without it the only route to the automatic width
// is hand-editing the workspace for a sentinel no user would guess.
void testDoubleClickingADividerRestoresTheAutomaticWidth()
{
    StubSource source;
    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());
    cfg.value_width = 300.0;

    scope::TablePanel table(cfg, source);
    sizedTable(table);
    expect(std::abs(table.columns().value_width - 300.0) < 0.01, "starts pinned");

    const double divider = table.columns().value_left - kDividerOffset;
    mouse(&table, QEvent::MouseButtonDblClick, QPointF(divider, 200.0), Qt::LeftButton,
          Qt::LeftButton);

    expect(table.getConfig().value_width < 0.0, "a double-click hands the column back to automatic");
    expect(std::abs(table.columns().value_width - 175.2) < 0.01,
           "and it sizes itself to the panel again");
}

// A press that is not on a divider must not start one. Every pixel of this panel
// that is not a divider belongs to the rows, and a drag that silently grabbed
// the nearest column would resize a layout the user was only clicking through.
void testAPressAwayFromADividerDoesNothing()
{
    StubSource source;
    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());
    cfg.show_age = false;

    scope::TablePanel table(cfg, source);
    sizedTable(table);

    const double before = table.columns().value_width;

    // The middle of the name column.
    mouse(&table, QEvent::MouseButtonPress, QPointF(100.0, 200.0), Qt::LeftButton, Qt::LeftButton);
    mouse(&table, QEvent::MouseMove, QPointF(40.0, 200.0), Qt::NoButton, Qt::LeftButton);
    mouse(&table, QEvent::MouseButtonRelease, QPointF(40.0, 200.0), Qt::LeftButton, Qt::NoButton);
    expect(table.columns().value_width == before, "a drag inside a column resizes nothing");

    // And where a HIDDEN column's divider would have been.
    mouse(&table, QEvent::MouseButtonPress, QPointF(kAgeLeft - kDividerOffset, 200.0),
          Qt::LeftButton, Qt::LeftButton);
    mouse(&table, QEvent::MouseMove, QPointF(kAgeLeft - 60.0, 200.0), Qt::NoButton,
          Qt::LeftButton);
    mouse(&table, QEvent::MouseButtonRelease, QPointF(kAgeLeft - 60.0, 200.0), Qt::LeftButton,
          Qt::NoButton);
    expect(table.getConfig().age_width < 0.0, "a hidden column has no divider to grab");
}

// Changing how the table LOOKS must not throw away what it has read. A rebind
// rebuilds every buffer, so without this a column dragged through
// `scope.panel_set_config` would empty every row -- and a cursor parked in the
// past would read "--" until the buffers refilled.
void testAWidthChangeDoesNotDiscardHistory()
{
    StubSource source;
    TablePanelConfig_t cfg;
    cfg.rows.push_back(rpmRow());

    scope::TablePanel table(cfg, source);
    feed(source, 0, {{98.0, 3000.0}, {99.5, 4200.0}});
    expect(table.stats().rows.at(0).retained == 2, "two samples are retained");

    TablePanelConfig_t wider = cfg;
    wider.value_width = 220.0;
    table.applyConfig(wider);

    expect(table.stats().rows.at(0).retained == 2,
           "a column width change keeps the history it had");
    expect(source.bound.size() == 1, "because it did not rebind at all");

    // But a changed BINDING must rebind, or the panel would be reading a signal
    // its config no longer names.
    TablePanelConfig_t repointed = cfg;
    repointed.rows[0].value_expression = "oilPressurePsi";
    table.applyConfig(repointed);

    expect(source.bound.size() == 2, "a changed row rebinds");
    expect(source.bound.back().value_expression == "oilPressurePsi", "onto the new expression");
    expect(table.stats().rows.at(0).retained == 0, "with the history honestly gone");
}

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

void runGestureTests()
{
    testTableColumnsSizeThemselvesUntilTheyAreSet();
    testDraggingADividerResizesThatColumn();
    testADragCannotSaveAWidthItCannotShow();
    testDoubleClickingADividerRestoresTheAutomaticWidth();
    testAPressAwayFromADividerDoesNothing();
    testAWidthChangeDoesNotDiscardHistory();
    testWheelZoomHoldsTheInstantUnderThePointer();
    testAZoomOnOnePanelMovesTheOther();
    testHoveringDoesNotPan();
    testAClickThatDoesNotTravelIsNotAPan();
    testDraggingPansTheSharedWindow();
    testShiftDragZoomsToTheBand();
    testShiftWheelTurnsOffAutoscale();
    testDoubleClickResumesFollowing();
}

}  // namespace panel_tests
