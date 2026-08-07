// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shared view window: zooming, panning, and what clamps it.
//
// This is where the window's whole navigation model lives, and almost every way
// it can be wrong LOOKS LIKE DATA rather than like a bug. A zoom that does not
// hold its anchor drifts the trace under the pointer; a pan that leaves the
// buffers behind draws the wrong stretch of a recording with no error anywhere;
// a live view that clamps against the right edge without re-arming simply stops
// scrolling, which reads as a dead publisher. None of those raise anything --
// they just draw a plausible wrong picture, which is exactly the class of
// failure the decimation and scrubbing tests exist for too.
//
// A stub DataSource, so no bus and no recording. QCoreApplication rather than
// QApplication: TimeBase is a QObject with a QTimer and needs no widgets, so
// this stays a plain unit test with no platform plugin.

#include "scope/data_source.h"
#include "scope/time_base.h"

#include <QCoreApplication>

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
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

void expectNear(double got, double want, double tolerance, const std::string& what)
{
    ++checks;
    if (!(std::abs(got - want) <= tolerance))
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s (got %.9f, wanted %.9f)\n", what.c_str(), got, want);
    }
}

// A source whose clock and capabilities the test drives directly. Binding is
// accepted and produces nothing: everything here is about the view window, and
// where samples come from is covered by the ring and recorded-source tests.
class StubSource : public scope::DataSource
{
  public:
    StubSource(bool live, bool seekable, double t_begin = 0.0, double t_end = 0.0) :
        live_(live), seekable_(seekable), t_begin_(t_begin), t_end_(t_end)
    {
    }

    scope::SourceCaps caps() const override
    {
        scope::SourceCaps caps;
        caps.live = live_;
        caps.seekable = seekable_;
        caps.t_begin = t_begin_;
        caps.t_end = t_end_;
        return caps;
    }

    std::vector<scope::TopicInfo> topics() const override { return {}; }
    std::uint64_t topicsRevision() const override { return 1; }

    scope::SignalHandle bind(const scope::SignalKey& /*key*/,
                             std::shared_ptr<scope::SignalBuffer> /*into*/) override
    {
        return 1;
    }
    void release(scope::SignalHandle /*handle*/) override {}

    double now() const override { return now_; }

    void seek(double t) override
    {
        now_ = t;
        seeks.push_back(t);
    }

    void setPlaying(bool playing) override { source_playing = playing; }
    void setRate(double rate) override { source_rate = rate; }

    void setNow(double t) { now_ = t; }

    // What the time base actually asked of it, which is the half a caller
    // cannot see from the view alone.
    std::vector<double> seeks;
    bool source_playing = false;
    double source_rate = 1.0;

  private:
    bool live_;
    bool seekable_;
    double t_begin_;
    double t_end_;
    double now_ = 0.0;
};

// ------------------------------------------------------------------ the basics

void testTheSpanIsExactlyTheWindow()
{
    StubSource source(true, false);
    source.setNow(100.0);
    scope::TimeBase time_base(source);

    time_base.setWindowSeconds(12.5);
    expectNear(time_base.windowSeconds(), 12.5, 1e-12, "the span is what was asked for");
    expectNear(time_base.viewEnd() - time_base.viewBegin(), time_base.windowSeconds(), 1e-12,
               "windowSeconds() is exactly viewEnd() - viewBegin()");
}

void testModeIsAWrapperOverFollowing()
{
    StubSource source(true, false);
    source.setNow(100.0);
    scope::TimeBase time_base(source);

    expect(time_base.mode() == scope::TimeBase::Mode::Live, "a new time base is live");
    expect(time_base.following(), "live means following");

    time_base.setMode(scope::TimeBase::Mode::Paused);
    expect(!time_base.following(), "pausing stops following");
    expect(time_base.mode() == scope::TimeBase::Mode::Paused, "and reports itself paused");

    time_base.setMode(scope::TimeBase::Mode::Live);
    expect(time_base.following(), "and back again");
    expect(time_base.mode() == scope::TimeBase::Mode::Live, "reported both ways round");
}

void testPausingFreezesTheViewWhereItWas()
{
    StubSource source(true, false);
    source.setNow(100.0);
    scope::TimeBase time_base(source);
    time_base.setWindowSeconds(10.0);
    time_base.setFollowing(true);

    // Following snapped the edge to now().
    expectNear(time_base.viewEnd(), 100.0, 1e-9, "a followed view sits at the source's clock");

    time_base.setMode(scope::TimeBase::Mode::Paused);
    source.setNow(140.0);

    // The frozen edge is the whole point: the source keeps advancing and the
    // view does not. Without a stored right edge this reads back as 140.
    expectNear(time_base.viewEnd(), 100.0, 1e-9, "a paused view does not move with the source");
    expectNear(time_base.viewBegin(), 90.0, 1e-9, "and keeps its span");
}

// ------------------------------------------------------------------- zooming

void testZoomHoldsItsAnchor()
{
    // Deliberately far from either edge of what is available, so nothing here
    // is clamped. Against a wall the anchor CANNOT hold -- the window has to
    // stop -- and mixing the two cases into one assertion would test the clamp
    // rather than the zoom.
    StubSource source(true, false);
    source.setNow(100000.0);
    scope::TimeBase time_base(source);
    time_base.setRetentionSeconds(100000.0);
    time_base.setWindowSeconds(60.0);
    time_base.setMode(scope::TimeBase::Mode::Paused);

    // THE assertion. Everything else about wheel zoom is chrome: what makes it
    // feel like a map rather than a slider is that the instant under the
    // pointer keeps its position in the window.
    for (const double fraction : {0.0, 0.25, 0.5, 0.75, 1.0})
    {
        for (const double factor : {0.25, 0.5, 2.0, 4.0})
        {
            time_base.setView(900.0, 960.0);
            const double anchor = 900.0 + fraction * 60.0;

            time_base.zoomAt(anchor, factor);

            const double span = time_base.windowSeconds();
            const double at = (anchor - time_base.viewBegin()) / span;
            expectNear(at, fraction, 1e-9,
                       "the anchor keeps its fraction of the window through a zoom");
            expectNear(span, 60.0 * factor, 1e-9, "and the span scaled by the factor");
        }
    }
}

void testZoomClampsWithoutLosingTheAnchor()
{
    StubSource source(true, false);
    source.setNow(1000.0);
    scope::TimeBase time_base(source);
    time_base.setRetentionSeconds(100000.0);
    time_base.setMode(scope::TimeBase::Mode::Paused);
    time_base.setView(900.0, 960.0);

    // Far past the minimum. A clamp that moved the anchor outside the window
    // would put the sample under the pointer off screen.
    for (int i = 0; i < 40; ++i)
    {
        time_base.zoomAt(930.0, 0.5);
    }
    expect(time_base.windowSeconds() >= 0.1, "the span does not collapse below the minimum");
    expect(time_base.viewBegin() <= 930.0 && 930.0 <= time_base.viewEnd(),
           "and the anchor is still inside the window");
}

void testZoomIgnoresNonsense()
{
    StubSource source(true, false);
    source.setNow(1000.0);
    scope::TimeBase time_base(source);
    time_base.setMode(scope::TimeBase::Mode::Paused);
    time_base.setView(900.0, 960.0);

    const double begin = time_base.viewBegin();
    const double end = time_base.viewEnd();

    // A panel one pixel wide -- which is what a dock being dragged looks like
    // for a frame -- can produce all three of these. A NaN reaching the view
    // poisons every panel's axis for the rest of the session, silently, because
    // painting a NaN draws nothing rather than failing.
    time_base.zoomAt(std::nan(""), 2.0);
    time_base.zoomAt(930.0, std::nan(""));
    time_base.zoomAt(930.0, 0.0);
    time_base.zoomAt(930.0, -1.0);

    expectNear(time_base.viewBegin(), begin, 1e-12, "a nonsense zoom leaves the view alone");
    expectNear(time_base.viewEnd(), end, 1e-12, "both edges of it");
}

// ------------------------------------------------------------------- panning

void testPanningStopsFollowing()
{
    StubSource source(true, false);
    source.setNow(1000.0);
    scope::TimeBase time_base(source);
    time_base.setRetentionSeconds(600.0);
    time_base.setWindowSeconds(30.0);

    time_base.panBy(-100.0);
    expect(!time_base.following(), "panning off the live edge stops following");
    expectNear(time_base.viewEnd(), 900.0, 1e-9, "and lands where it was told");
}

void testPanningBackClampsAtTheRetainedEdge()
{
    StubSource source(true, false);
    source.setNow(1000.0);
    scope::TimeBase time_base(source);
    time_base.setRetentionSeconds(300.0);
    time_base.setWindowSeconds(30.0);

    time_base.panBy(-1e9);

    // Past what the buffers hold there is nothing to draw, and empty space at
    // the left of a plot is indistinguishable from a publisher that had not
    // started yet.
    expectNear(time_base.viewBegin(), 700.0, 1e-9, "a live view stops at the retained edge");
    expectNear(time_base.windowSeconds(), 30.0, 1e-9, "and keeps its span rather than squashing");
}

void testPanningToTheLiveEdgeReArmsFollowing()
{
    StubSource source(true, false);
    source.setNow(1000.0);
    scope::TimeBase time_base(source);
    time_base.setRetentionSeconds(600.0);
    time_base.setWindowSeconds(30.0);

    time_base.panBy(-100.0);
    expect(!time_base.following(), "panned away, not following");

    // Dragging further right than there is data. Without the re-arm the view
    // pins itself at now() with following off, and then never moves again --
    // which on screen is a plot that has stopped scrolling, i.e. exactly what a
    // dead publisher looks like.
    time_base.panBy(1e9);
    expect(time_base.following(), "panning back to the live edge resumes following");
    expectNear(time_base.viewEnd(), 1000.0, 1e-9, "at the source's clock");
}

void testWideningAWindowDoesNotStopALiveView()
{
    StubSource source(true, false);
    source.setNow(1000.0);
    scope::TimeBase time_base(source);
    time_base.setRetentionSeconds(600.0);

    time_base.setWindowSeconds(120.0);

    // The spin box and the preset list are "show me the last N seconds", which
    // is not the same gesture as grabbing the window and dragging it.
    expect(time_base.following(), "setting the span leaves a live view following");
    expectNear(time_base.viewEnd(), 1000.0, 1e-9, "with the right edge still at now");
}

// -------------------------------------------------------- a seekable source

void testTheViewsRightEdgeIsThePlayhead()
{
    StubSource source(false, true, 0.0, 600.0);
    source.setNow(600.0);
    scope::TimeBase time_base(source);
    time_base.setWindowSeconds(30.0);

    source.seeks.clear();
    time_base.setView(100.0, 130.0);

    expect(source.seeks.size() == 1, "moving the view seeks exactly once");
    if (!source.seeks.empty())
    {
        expectNear(source.seeks.back(), 130.0, 1e-9, "to the view's RIGHT edge");
    }

    // The rule this pins: RecordedSource loads [t - history, t] around ONE
    // position, so a view sitting anywhere other than the playhead would be
    // drawn from buffers holding a different stretch of the recording -- and it
    // would look like data, not like a bug.
    expectNear(time_base.viewEnd(), source.now(), 1e-9,
               "the view's right edge and the playhead are the same thing");
}

void testAGestureStopsPlayback()
{
    StubSource source(false, true, 0.0, 600.0);
    source.setNow(100.0);
    scope::TimeBase time_base(source);

    time_base.setPlaying(true);
    expect(time_base.playing(), "playing");

    time_base.panBy(-10.0);
    expect(!time_base.playing(), "a pan stops playback");
    expect(!source.source_playing, "and the source was told");

    // Otherwise playback drags the view off the span just chosen, at the render
    // rate, which reads as a window that will not stay where it is put.
}

void testPlayingResumesFollowing()
{
    StubSource source(false, true, 0.0, 600.0);
    source.setNow(300.0);
    scope::TimeBase time_base(source);
    time_base.setWindowSeconds(30.0);

    time_base.panBy(-100.0);
    expect(!time_base.following(), "panned away from the playhead");

    time_base.setPlaying(true);
    expect(time_base.following(),
           "pressing play makes the view ride the playhead again");

    // Without this the head advances under a window that stays put: the trace
    // sits still while the position readout climbs.
}

void testSeekMovesTheWholeWindow()
{
    StubSource source(false, true, 0.0, 600.0);
    source.setNow(600.0);
    scope::TimeBase time_base(source);
    time_base.setWindowSeconds(20.0);

    time_base.seek(250.0);
    expectNear(time_base.viewEnd(), 250.0, 1e-9, "seek puts the right edge where it was told");
    expectNear(time_base.viewBegin(), 230.0, 1e-9, "and carries the span with it");
}

void testSeekingIsClampedToTheRecording()
{
    StubSource source(false, true, 10.0, 60.0);
    source.setNow(60.0);
    scope::TimeBase time_base(source);
    time_base.setWindowSeconds(5.0);

    time_base.seek(1e9);
    expectNear(time_base.viewEnd(), 60.0, 1e-9, "cannot seek past the end");

    time_base.seek(-1e9);
    expectNear(time_base.viewBegin(), 10.0, 1e-9, "nor before the beginning");

    // A window wider than the recording narrows to it rather than hanging off
    // an edge that has no data behind it.
    time_base.setWindowSeconds(3600.0);
    expectNear(time_base.viewBegin(), 10.0, 1e-9, "a too-wide window starts at the recording");
    expectNear(time_base.viewEnd(), 60.0, 1e-9, "and ends at it");
}

void testInteractionCoalescesSeeks()
{
    StubSource source(false, true, 0.0, 600.0);
    source.setNow(600.0);
    scope::TimeBase time_base(source);
    time_base.setWindowSeconds(30.0);

    source.seeks.clear();
    time_base.setInteracting(true);

    // A drag emits one of these per pass of the event loop. Each seek refills a
    // whole retention window per bound signal, so seeking per event makes a drag
    // stutter in proportion to how much history is retained -- the opposite of
    // what retaining more should cost.
    for (int i = 0; i < 50; ++i)
    {
        time_base.panBy(-1.0);
    }
    expect(source.seeks.empty(), "no seek reaches the source mid-drag");

    time_base.setInteracting(false);
    expect(source.seeks.size() == 1, "ending the drag applies exactly one");
    if (!source.seeks.empty())
    {
        expectNear(source.seeks.back(), time_base.viewEnd(), 1e-9,
                   "and it is where the view finished");
    }
}

void testASeekOutsideAnInteractionIsImmediate()
{
    StubSource source(false, true, 0.0, 600.0);
    source.setNow(600.0);
    scope::TimeBase time_base(source);

    source.seeks.clear();
    time_base.seek(120.0);

    // The agent interface sets the view and then reads sample_stats in the same
    // call. Deferring this one to the render tick would report the buffers from
    // before the seek, which is a wrong answer rather than a slow one.
    expect(source.seeks.size() == 1, "a seek outside a drag reaches the source at once");
}

// ------------------------------------------------------------------- fitting

void testFitOnARecording()
{
    StubSource source(false, true, 12.0, 90.0);
    source.setNow(90.0);
    scope::TimeBase time_base(source);
    time_base.setWindowSeconds(5.0);

    time_base.fitAll();
    expectNear(time_base.viewBegin(), 12.0, 1e-9, "fit starts at the recording");
    expectNear(time_base.viewEnd(), 90.0, 1e-9, "and ends at it");
}

void testFitOnALiveSourceIsTheRetainedWindow()
{
    StubSource source(true, false);
    source.setNow(1000.0);
    scope::TimeBase time_base(source);
    time_base.setRetentionSeconds(240.0);

    time_base.fitAll();
    expectNear(time_base.windowSeconds(), 240.0, 1e-9,
               "fitting a live source shows everything retained");
    expect(time_base.following(),
           "and it is against the live edge, so it keeps following");
}

// ---------------------------------------------------------------- the source

void testSwappingSourcesResetsThePositionAndKeepsTheSpan()
{
    StubSource live(true, false);
    live.setNow(1000.0);
    scope::TimeBase time_base(live);
    time_base.setRetentionSeconds(600.0);
    time_base.setWindowSeconds(15.0);
    time_base.setCursor(995.0);
    time_base.panBy(-200.0);

    StubSource recording(false, true, 0.0, 400.0);
    recording.setNow(0.0);
    time_base.setSource(recording);

    // A live source counts seconds since it was constructed and a recorded one
    // since the recording began, so a position carried across lands somewhere
    // arbitrary -- silently, because both are just doubles.
    expect(time_base.following(), "a new source starts followed");
    expect(!time_base.cursor().has_value(), "and with no cursor from the old epoch");
    expect(!time_base.playing(), "and stopped");

    // The span is a preference rather than a position: someone reviewing at a
    // 15-second window wants one in the next recording too.
    expectNear(time_base.windowSeconds(), 15.0, 1e-9, "the span survives the swap");
}

void testRetentionShrinkingRepositionsALiveView()
{
    StubSource source(true, false);
    source.setNow(1000.0);
    scope::TimeBase time_base(source);
    time_base.setRetentionSeconds(600.0);
    time_base.setWindowSeconds(60.0);
    time_base.panBy(-400.0);
    expectNear(time_base.viewBegin(), 540.0, 1e-9, "panned back inside the old retention");

    // Lowering the workspace's history_seconds throws away what the buffers
    // held, so a view over that stretch is now over nothing.
    time_base.setRetentionSeconds(120.0);
    expect(time_base.viewBegin() >= 880.0 - 1e-9,
           "shrinking retention pulls the view back inside what is retained");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    spdlog::set_level(spdlog::level::off);

    testTheSpanIsExactlyTheWindow();
    testModeIsAWrapperOverFollowing();
    testPausingFreezesTheViewWhereItWas();

    testZoomHoldsItsAnchor();
    testZoomClampsWithoutLosingTheAnchor();
    testZoomIgnoresNonsense();

    testPanningStopsFollowing();
    testPanningBackClampsAtTheRetainedEdge();
    testPanningToTheLiveEdgeReArmsFollowing();
    testWideningAWindowDoesNotStopALiveView();

    testTheViewsRightEdgeIsThePlayhead();
    testAGestureStopsPlayback();
    testPlayingResumesFollowing();
    testSeekMovesTheWholeWindow();
    testSeekingIsClampedToTheRecording();
    testInteractionCoalescesSeeks();
    testASeekOutsideAnInteractionIsImmediate();

    testFitOnARecording();
    testFitOnALiveSourceIsTheRetainedWindow();

    testSwappingSourcesResetsThePositionAndKeepsTheSpan();
    testRetentionShrinkingRepositionsALiveView();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
