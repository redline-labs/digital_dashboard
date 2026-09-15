// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "test_panels_common.h"

// The window chrome: the toolbar reusing the menu's actions, the mode
// toggle reading the source, and the overview strip's hit tests.

namespace panel_tests
{

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

void testTheModeToggleFollowsTheSourceNotTheClick()
{
    scope::ScopeWindow window;

    auto* toggle = window.findChild<QToolButton*>("mode_toggle");
    auto* action = window.findChild<QAction*>("action_online");
    expect(toggle != nullptr && action != nullptr, "the mode control exists");
    if (toggle == nullptr || action == nullptr)
    {
        return;
    }

    expect(!toggle->isChecked(), "a fresh window is offline");
    expect(!window.isOnline(), "and says so");
    expect(toggle->text() == QStringLiteral("Offline"),
           "the button reads the state it is in, not the action it performs");

    // Swapped WITHOUT going near the control, which is what --bag at startup and
    // the agent interface both do. A button tracking only its own clicks would
    // still be claiming whatever was last pressed.
    window.setSource(std::make_unique<SeekableStub>());

    expect(!toggle->isChecked(), "a recording swapped in from elsewhere is still offline");
    expect(!action->isChecked(), "and the menu item agrees with the button");

    // The live case, from the same direction. Nothing here touches the toggle.
    window.setSource(std::make_unique<StubSource>());

    expect(toggle->isChecked(), "a live source swapped in from elsewhere checks the toggle");
    expect(action->isChecked(), "and the menu item follows it");
    expect(toggle->text() == QStringLiteral("● Online"), "and the label follows too");
}

void testAFreshWindowIsOfflineWithNothingLoaded()
{
    // The default that everything else here depends on. A window that opened a
    // zenoh session before anyone asked would make "Offline" a label rather than
    // a fact -- and there is no assertion that can see a session from here, so
    // this pins the observable consequences instead: no capture, nothing to
    // review, and a source that is neither live nor seekable.
    scope::ScopeWindow window;

    expect(!window.isOnline(), "a fresh window is offline");
    expect(window.recorder() == nullptr, "with no recorder at all");
    expect(!window.hasCapture(), "and nothing to review");

    const scope::SourceCaps caps = window.source().caps();
    expect(!caps.live && !caps.seekable, "over a source that is neither live nor seekable");
    expect(window.source().topics().empty(), "offering no topics");

    // Both actions act on a capture that does not exist. Enabled, they would
    // answer a click with a status-bar line, which reads as a broken app rather
    // than an unavailable one -- and on a freshly started window that was every
    // click.
    auto* review = window.findChild<QAction*>("action_review_capture");
    auto* save = window.findChild<QAction*>("action_save_recording");
    expect(review != nullptr && !review->isEnabled(), "Review Session Capture is disabled");
    expect(save != nullptr && !save->isEnabled(), "and so is Save Recording");

    // The landing screen outranks the panel hint: adding a panel over nothing
    // draws an empty plot, which looks exactly like a signal that is not
    // publishing.
    auto* offline_hint = window.findChild<QWidget*>("offline_hint");
    auto* empty_hint = window.findChild<QLabel*>("empty_hint");
    expect(offline_hint != nullptr && offline_hint->isVisibleTo(&window),
           "the offline landing screen is shown");
    expect(empty_hint != nullptr && !empty_hint->isVisibleTo(&window),
           "and the no-panels hint is not, because it is the lesser problem");
}

void testTheOfflineHintDoesNotSqueezeThePanels()
{
    // QMainWindow honours the central widget's minimum before it gives anything
    // to the docks, so a hint wide enough to read costs every plot in the window
    // the width it needs. This is not hypothetical: growing the hint from one
    // label into a label and two buttons squeezed the panels from 637 px to 160
    // and broke three geometry tests on mouse positions that no longer landed
    // where they used to.
    //
    // Measured on the PANEL, not on the hint, because the panel is what the
    // squeeze costs and what every geometry test downstream depends on. The
    // window is 1280 wide and the signal browser takes ~260 of it, so anything
    // above 400 means the hint is not the thing deciding the layout.
    scope::ScopeWindow window;
    scope::TimeSeriesPanel* panel = readyPanel(window, "probe");

    auto* offline_hint = window.findChild<QWidget*>("offline_hint");
    expect(offline_hint != nullptr && !offline_hint->isVisibleTo(&window),
           "the offline hint yields the central area once there are panels");
    expect(panel->width() > 400, "so a panel keeps a usable width");
}

void testGoingOfflineLandsOnTheSessionCapture()
{
    // The round trip, with the capture stubbed out of the picture: going online
    // needs a bus, which this test does not have, so the transition is driven
    // through setSource() the way --bag and the agent interface drive it, and
    // what is checked is the part that does not need one.
    scope::ScopeWindow window;

    // No capture, so going offline from a live source has nothing to land on and
    // must land on an EMPTY source rather than on a recording of nothing.
    window.setSource(std::make_unique<StubSource>());
    expect(window.isOnline(), "the stub live source is online");

    window.goOffline();

    expect(!window.isOnline(), "going offline leaves the bus");
    const scope::SourceCaps caps = window.source().caps();
    expect(!caps.seekable, "and lands on nothing rather than on a recording of nothing");

    auto* review = window.findChild<QAction*>("action_review_capture");
    expect(review != nullptr && !review->isEnabled(),
           "with Review still disabled, because there is still no capture");
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

void testAStripDragCoalescesToOneSeek()
{
    // The strip's viewRequested lands in TimeBase::setView, and seeks coalesce
    // to flushSeek() unconditionally -- so however many mouse-moves a drag
    // emits, the source refills its retention window once per render tick, not
    // once per event.
    std::unique_ptr<scope::OverviewStrip> strip(readyStrip());

    SeekableStub source;
    scope::TimeBase time_base(source);
    QObject::connect(strip.get(), &scope::OverviewStrip::viewRequested, &time_base,
                     &scope::TimeBase::setView);

    source.seeks.clear();
    mouse(strip.get(), QEvent::MouseButtonPress, QPointF(500.0, 20.0), Qt::LeftButton,
          Qt::LeftButton);
    for (int x = 505; x <= 600; x += 5)
    {
        mouse(strip.get(), QEvent::MouseMove, QPointF(x, 20.0), Qt::NoButton, Qt::LeftButton);
    }
    mouse(strip.get(), QEvent::MouseButtonRelease, QPointF(600.0, 20.0), Qt::LeftButton,
          Qt::NoButton);

    expect(source.seeks.empty(), "no seek reaches the source during the drag");
    time_base.flushSeek();
    expect(source.seeks.size() == 1, "the render tick's flush applies exactly one");
}

void testTheScrubberDragCoalescesThroughTheTimeBase()
{
    // The video panel's own seek bar. Its seekRequested lands in
    // TimeBase::seek, and coalescing happens there unconditionally -- so a
    // drag's 60-125 events a second become one source seek per flush, each of
    // which is a file read on a recorded source.
    scope::VideoScrubber scrubber;
    scrubber.resize(400, 18);
    scrubber.setExtent(0.0, 100.0);
    scrubber.setSeekable(true);

    SeekableStub source;
    scope::TimeBase time_base(source);
    QObject::connect(&scrubber, &scope::VideoScrubber::seekRequested, &time_base,
                     &scope::TimeBase::seek);

    std::vector<double> seeks;
    QObject::connect(&scrubber, &scope::VideoScrubber::seekRequested,
                     [&](double t) { seeks.push_back(t); });

    source.seeks.clear();
    mouse(&scrubber, QEvent::MouseButtonPress, QPointF(100.0, 9.0), Qt::LeftButton,
          Qt::LeftButton);
    for (int x = 110; x <= 200; x += 10)
    {
        mouse(&scrubber, QEvent::MouseMove, QPointF(x, 9.0), Qt::NoButton, Qt::LeftButton);
    }
    mouse(&scrubber, QEvent::MouseButtonRelease, QPointF(200.0, 9.0), Qt::LeftButton,
          Qt::NoButton);

    expect(seeks.size() > 2, "scrubber: the drag really did emit many seeks");
    expect(!seeks.empty() && seeks.back() > seeks.front(),
           "scrubber: dragging right moves the requested time forward");
    expect(source.seeks.empty(), "scrubber: none of them reached the source mid-drag");
    time_base.flushSeek();
    expect(source.seeks.size() == 1, "scrubber: the flush applies exactly one");

    // A scrubber over a live source has nothing to seek to, so it must not
    // pretend. A bar that looks draggable and does nothing reads as broken.
    scope::VideoScrubber live;
    live.resize(400, 18);
    live.setExtent(0.0, 100.0);
    live.setSeekable(false);

    int live_seeks = 0;
    QObject::connect(&live, &scope::VideoScrubber::seekRequested,
                     [&](double) { ++live_seeks; });
    mouse(&live, QEvent::MouseButtonPress, QPointF(100.0, 9.0), Qt::LeftButton, Qt::LeftButton);
    mouse(&live, QEvent::MouseButtonRelease, QPointF(100.0, 9.0), Qt::LeftButton, Qt::NoButton);
    expect(live_seeks == 0, "scrubber: an unseekable source produces no seeks at all");
}

// THE BUG THIS WHOLE SEAM EXISTS FOR. toWorkspace() used to qobject_cast to
// TimeSeriesPanel with no else, so any other panel type saved its `type:` with
// its config left on monostate -- which the YAML encoder then omits entirely, so
// the panel came back default-constructed. Every setting lost on every save,
// with nothing logged.
void testTheWorkspaceKeepsAVideoPanelsConfig()
{
    scope::ScopeWindow window;

    const QString id = window.addPanel(scope::panel_type_t::video, "cam");
    expect(id == "cam", "a video panel was added to a real window");

    scope::ScopeWindow::PanelEntry* entry = window.findPanel("cam");
    expect(entry != nullptr, "and can be found again");
    if (entry == nullptr)
    {
        return;
    }

    VideoPanelConfig_t configured;
    configured.title = "Dash cam";
    configured.zenoh_key = "nodes/carplay/video";
    configured.retention_seconds = 45.0;
    configured.max_buffer_bytes = 64ull * 1024 * 1024;
    configured.show_scrubber = false;
    expect(scope::applyPanelConfig(*entry->panel, configured),
           "the panel accepted a configuration");

    const scope::scope_workspace_t saved = window.toWorkspace();
    expect(saved.panels.size() == 1, "the workspace holds the panel");
    if (saved.panels.empty())
    {
        return;
    }

    expect(saved.panels[0].type == scope::panel_type_t::video,
           "it saved as a video panel");
    expect(!std::holds_alternative<std::monostate>(saved.panels[0].config),
           "AND ITS CONFIG IS NOT MONOSTATE -- the whole bug");

    const auto* stored = std::get_if<VideoPanelConfig_t>(&saved.panels[0].config);
    expect(stored != nullptr, "the config is the video kind");
    if (stored == nullptr)
    {
        return;
    }

    expect(stored->title == "Dash cam", "the title survived the save");
    expect(stored->zenoh_key == "nodes/carplay/video", "the bound key survived");
    expect(stored->retention_seconds == 45.0, "the retention survived");
    expect(stored->max_buffer_bytes == 64ull * 1024 * 1024, "the byte bound survived");
    expect(!stored->show_scrubber, "a non-default bool survived");
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

void runChromeTests()
{
    testTheScrubberDragCoalescesThroughTheTimeBase();
    testTheWorkspaceKeepsAVideoPanelsConfig();
    testTimeBaseClampsSillyValues();
    testTheStripHitTestsEdgesBeforeTheBody();
    testTheStripBodyDragPansWithoutZooming();
    testTheStripKeepsTheGrabOffset();
    testClickingOutsideTheRegionCentresTheView();
    testAStripDragCoalescesToOneSeek();
    testTheStripReplacedTheScrubber();
    testTheToolbarReusesTheMenusActions();
    testTheToolbarOffersEveryPanelType();
    testTheModeToggleFollowsTheSourceNotTheClick();
    testAFreshWindowIsOfflineWithNothingLoaded();
    testTheOfflineHintDoesNotSqueezeThePanels();
    testGoingOfflineLandsOnTheSessionCapture();
    testPauseFollowsAPanRatherThanOnlyItsOwnClicks();
    testNavigationActionsMoveTheSharedWindow();
}

}  // namespace panel_tests
