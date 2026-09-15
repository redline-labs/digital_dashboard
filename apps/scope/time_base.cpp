#include "scope/time_base.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace scope
{

namespace
{

// A window narrower than this is not a plot, and one wider than a day makes the
// tick labelling meaningless. Both are clamps rather than errors: the caller is
// a spin box or a workspace file, and refusing to load a config over a silly
// number is worse than loading it with a sane one and saying so.
constexpr double kMinWindowSeconds = 0.1;
constexpr double kMaxWindowSeconds = 24.0 * 60.0 * 60.0;

// Above the display refresh rate there is nothing to gain, and the dashboard
// has already been bitten by the other end: an update rate over 1000 became a
// 0 ms timer that fired on every pass of the event loop.
constexpr int kMinRenderRateHz = 1;
constexpr int kMaxRenderRateHz = 120;

// Both ends of the playback rate are useful: 0.1x to study a transient, 20x to
// find one. Zero is excluded because that is what setPlaying(false) means, and
// a rate of zero that stopped the head would be a second, silent way of pausing.
constexpr double kMinRate = 0.05;
constexpr double kMaxRate = 50.0;

// How far the view's right edge has to be from the source's position before
// moving it counts as a seek. A microsecond is far below anything a recording
// resolves and far above the rounding in a span subtraction, so it separates
// "the user moved the window" from "the window is riding the playhead".
constexpr double kSeekEpsilon = 1e-6;

}  // namespace

TimeBase::TimeBase(DataSource& source, QObject* parent) : QObject(parent), source_(&source)
{
    timer_.setObjectName("scope_render_timer");
    connect(&timer_, &QTimer::timeout, this, [this]() {
        // Whatever the gestures since the last tick asked for, ONCE, before the
        // source is ticked -- so a frame is drawn from buffers that already
        // hold the position the view is claiming. This is the whole seek
        // coalescing mechanism: however many wheel events or drag moves landed
        // in the last 33 ms, the retention window refills once.
        flushSeek();

        // The source first: a playing recorded source advances its position and
        // refills the bound buffers here, and the panels drain them on frame().
        // The other order would draw each tick one frame behind the head.
        source_->tick();

        // Playback stops at the end of the recording rather than sitting there
        // still claiming to run. Decided HERE because this owns the flag the
        // transport bar renders from -- a source that cleared its own would
        // leave the Play button pressed with nothing moving. Looping, when it
        // arrives, belongs here too for the same reason.
        if (playing_)
        {
            const SourceCaps caps = source_->caps();
            if (source_->now() >= caps.t_end)
            {
                setPlaying(false);
            }
        }

        emit frame();
    });
    restartTimer();
}

TimeBase::~TimeBase() = default;

void TimeBase::setSource(DataSource& source)
{
    if (&source == source_)
    {
        return;
    }
    source_ = &source;

    // Both of these are values on the OLD source's epoch, and nothing about
    // them survives the move: a live source counts seconds since it was
    // constructed, a recorded one seconds since the recording began. Kept, a
    // frozen right edge would land somewhere arbitrary in the new source and
    // the shared cursor would read out an instant that does not exist -- and
    // neither would look wrong, because both are just doubles.
    cursor_.reset();

    // A new source starts stopped. Dropping into a recording that immediately
    // began playing would move the window before anyone could look at it.
    playing_ = false;
    source_->setPlaying(false);

    // Following, so the right edge is whatever the new source's clock says and
    // no value from the old epoch survives. The SPAN does, because it is a
    // preference rather than a position: someone reviewing at a 5-second window
    // wants one in the next recording too.
    follow_ = true;
    view_end_ = source_->now();
    pending_seek_.reset();

    // The rate is a POSITION-like thing, not a preference like the span: a
    // fresh source starts at 1.0x (RecordedSource's own default), and a 20x
    // left over from the previous recording would make this object report a
    // rate the source is not honouring until the next setRate().
    rate_ = 1.0;

    emit changed();
    emit cursorMoved();
}

void TimeBase::setWindowSeconds(double seconds)
{
    const double clamped = std::clamp(seconds, kMinWindowSeconds, kMaxWindowSeconds);
    if (clamped != seconds)
    {
        SPDLOG_WARN("Window of {}s is outside [{}, {}]; using {}s.", seconds, kMinWindowSeconds,
                    kMaxWindowSeconds, clamped);
    }

    const bool span_changed = clamped != window_seconds_;

    // Zoom about the RIGHT edge. "Show me the last N seconds" is what this
    // control has always meant, and it is the one zoom that should NOT stop a
    // live view from following -- widening the window while tailing the bus is
    // not the same gesture as grabbing it and dragging.
    const double end = viewEnd();
    if (follow_)
    {
        applyView(end - clamped, end);
        emit changed();
    }
    else
    {
        setView(end - clamped, end);
    }
    if (span_changed)
    {
        emit persistentChanged();
    }
}

void TimeBase::setRenderRateHz(int hz)
{
    const int clamped = std::clamp(hz, kMinRenderRateHz, kMaxRenderRateHz);
    if (clamped != hz)
    {
        SPDLOG_WARN("Render rate of {} Hz is outside [{}, {}]; using {} Hz.", hz, kMinRenderRateHz,
                    kMaxRenderRateHz, clamped);
    }
    if (clamped == render_rate_hz_)
    {
        return;
    }
    render_rate_hz_ = clamped;
    restartTimer();
    emit changed();
    emit persistentChanged();
}

// ----------------------------------------------------------------- the view

std::pair<double, double> TimeBase::availableRange() const
{
    const SourceCaps caps = source_->caps();
    if (caps.seekable)
    {
        // A recording that is a single instant, or one still being written and
        // not yet containing two messages, would otherwise give a zero-width
        // range that every clamp below divides by.
        if (caps.t_end > caps.t_begin)
        {
            return {caps.t_begin, caps.t_end};
        }
        return {caps.t_begin, caps.t_begin + kMinWindowSeconds};
    }

    // A bus has no beginning, so caps() has no meaningful t_begin. What bounds
    // a live view is what the panels' buffers still hold: scrolling back past
    // that shows emptiness indistinguishable from a publisher that had not
    // started yet.
    //
    // NOT floored at the source's epoch, however tempting. A live source's
    // clock starts at zero, so flooring here means that thirty seconds into a
    // session no window wider than thirty seconds can exist -- setWindowSeconds
    // silently clamps to the uptime, and the default 30 s view is unusable for
    // the first half minute of every run. Empty space to the left of a young
    // trace is the honest picture and always was.
    //
    // The strip's histogram does need a floor, because it can only be COUNTED
    // from the epoch forward. That belongs at the point of counting, not here:
    // see ScopeWindow::refreshDensity().
    const double now = source_->now();
    return {now - retention_seconds_, now};
}

void TimeBase::applyView(double begin, double end)
{
    // NaN in, and every panel's axis is poisoned for the rest of the session --
    // the arithmetic below propagates it, the clamps do not reject it, and
    // painting silently draws nothing. A degenerate widget asking for a zoom
    // about a zero-width rect is the way in, so it is refused here rather than
    // at each of the callers.
    if (!std::isfinite(begin) || !std::isfinite(end))
    {
        SPDLOG_WARN("Ignoring a non-finite view window [{}, {}].", begin, end);
        return;
    }

    double span = std::clamp(end - begin, kMinWindowSeconds, kMaxWindowSeconds);

    const auto [low, high] = availableRange();
    const double available = high - low;

    // Narrow only when the range itself is narrower. Otherwise the window keeps
    // the width it was asked for and SLIDES into range -- squashing it instead
    // would make a pan near the edge silently change the zoom level.
    if (span > available)
    {
        span = std::max(available, kMinWindowSeconds);
    }

    double new_end = end;
    if (new_end > high)
    {
        new_end = high;
    }
    if (new_end - span < low)
    {
        // THE RIGHT EDGE IS THE PLAYHEAD ON A SEEKABLE SOURCE, so it has to be
        // able to reach the beginning of the recording -- and it cannot if the
        // whole window is required to fit inside the range.
        //
        // Sliding the window right instead makes the first `span` seconds
        // unreachable: seek(t_begin) lands the head at t_begin + span, which on
        // a 110 s recording at the default 30 s window parks it a quarter of the
        // way in and reads as a scrub bar that rails a third from the left. The
        // video panel makes it plainest -- the picture it shows IS the right
        // edge, so the opening half-minute could not be looked at at all except
        // by watching playback go past it.
        //
        // The window is therefore allowed to hang off the left of the recording,
        // which draws as empty space before t_begin. That is the honest picture,
        // and the same call availableRange() already makes for a live source
        // younger than the window.
        //
        // A LIVE SOURCE STILL SLIDES. There `low` is now - retention rather than
        // the start of anything, so a window hanging off it would show emptiness
        // that is indistinguishable from a publisher which had not started --
        // and nothing there is a playhead that has to reach a first frame.
        new_end = source_->caps().seekable ? std::max(new_end, low) : low + span;
    }

    if (span == window_seconds_ && new_end == view_end_)
    {
        return;
    }

    window_seconds_ = span;
    view_end_ = new_end;

    // The right edge IS the playhead on a seekable source -- see the seek()
    // comment in the header. Moving the view without telling the source would
    // leave the buffers holding a different stretch of the recording than the
    // one being drawn.
    //
    // ONLY WHEN THE VIEW HAS MOVED AWAY FROM WHERE THE SOURCE ALREADY IS. While
    // playback is running, the follow branch of the render tick sets the right
    // edge to exactly source_->now() every frame; seeking to that would command
    // the source back to the position it just advanced from, thirty times a
    // second, and playback would stall while looking like it was running.
    if (source_->caps().seekable && std::abs(view_end_ - source_->now()) > kSeekEpsilon)
    {
        // Parked, not applied: the render tick (or an agent dispatch) flushes
        // it. Applying it here made every wheel event a full refill of every
        // bound signal's retention window.
        pending_seek_ = view_end_;
    }
}

void TimeBase::flushSeek()
{
    if (!pending_seek_)
    {
        return;
    }
    const double t = *pending_seek_;
    pending_seek_.reset();

    if (source_->caps().seekable)
    {
        source_->seek(t);
    }
}

void TimeBase::setView(double begin, double end)
{
    // A deliberate move by the user, so the window stops being driven for them.
    // Both halves matter: a live view pinned to now cannot be held still, and
    // playback would drag a recorded view off the span just chosen, at the
    // render rate.
    //
    // The capture BEFORE clearing follow_ is what freezes the view where it is:
    // view_end_ is stale while following, so dropping the flag first would snap
    // the window back to wherever it was last parked.
    view_end_ = viewEnd();
    follow_ = false;
    if (playing_)
    {
        setPlaying(false);
    }

    applyView(begin, end);

    // RE-ARM AT THE WALL. Panning right on a live source clamps the right edge
    // to now() -- and with following off, the next tick does not move it, so
    // the plot freezes at the one position where it should obviously still be
    // scrolling. It reads as a hang, and it is the state a user lands in by
    // dragging further than there is data.
    //
    // Where the clamp put the edge exactly at now(), the followed and unfollowed
    // states are indistinguishable in every respect except that one of them is
    // dead, so this picks the live one. Any view that did NOT end up against the
    // wall stays put, which is the whole point of panning.
    if (!source_->caps().seekable && std::abs(view_end_ - source_->now()) <= kSeekEpsilon)
    {
        follow_ = true;
    }

    emit changed();
}

void TimeBase::zoomAt(double anchor_t, double factor)
{
    if (!std::isfinite(anchor_t) || !std::isfinite(factor) || factor <= 0.0)
    {
        return;
    }

    const double span = window_seconds_;
    if (span <= 0.0)
    {
        return;
    }

    // Where the anchor sits in the window, as a fraction. Holding that fraction
    // fixed is what keeps the sample under the mouse under the mouse -- the
    // property that makes wheel-zoom feel like a map rather than like a slider.
    // Clamped because a drag can carry the pointer off the plot, and an anchor
    // outside the window would push it somewhere nobody asked for.
    const double at = std::clamp((anchor_t - viewBegin()) / span, 0.0, 1.0);
    const double new_span = std::clamp(span * factor, kMinWindowSeconds, kMaxWindowSeconds);

    setView(anchor_t - at * new_span, anchor_t + (1.0 - at) * new_span);
}

void TimeBase::panBy(double dt)
{
    if (!std::isfinite(dt) || dt == 0.0)
    {
        return;
    }
    setView(viewBegin() + dt, viewEnd() + dt);
}

void TimeBase::fitAll()
{
    const auto [low, high] = availableRange();
    setView(low, high);
}

void TimeBase::setRetentionSeconds(double seconds)
{
    if (seconds <= 0.0 || retention_seconds_ == seconds)
    {
        return;
    }
    retention_seconds_ = seconds;

    // Re-clamp: shrinking retention can leave the current view partly outside
    // what the buffers still answer for.
    applyView(viewBegin(), viewEnd());
    emit changed();
    emit persistentChanged();
}

void TimeBase::setFollowing(bool on)
{
    if (on == follow_)
    {
        return;
    }

    // Stopping: capture where the derived edge had got to, or the view jumps
    // back to whatever view_end_ was last left holding. Starting: nothing to
    // compute, because viewEnd() begins deriving from the source again.
    if (!on)
    {
        view_end_ = viewEnd();
    }
    follow_ = on;

    emit changed();
}

// ------------------------------------------------------------------- playback

void TimeBase::seek(double t)
{
    if (!source_->caps().seekable)
    {
        return;
    }

    // Move the WINDOW so its right edge lands on `t`, keeping the span, and let
    // applyView() do the clamping and the source_->seek(). Seeking the source
    // directly here and leaving the view alone was the old shape and is now
    // wrong: the two would describe different stretches of the recording.
    setView(t - window_seconds_, t);
}

void TimeBase::setRate(double rate)
{
    const double clamped = std::clamp(rate, kMinRate, kMaxRate);
    if (clamped != rate)
    {
        SPDLOG_WARN("Playback rate of {}x is outside [{}, {}]; using {}x.", rate, kMinRate,
                    kMaxRate, clamped);
    }
    if (clamped == rate_)
    {
        return;
    }
    rate_ = clamped;
    source_->setRate(rate_);
    emit changed();
}

void TimeBase::setPlaying(bool playing)
{
    if (!source_->caps().seekable)
    {
        return;
    }
    if (playing == playing_)
    {
        return;
    }
    playing_ = playing;

    // Playing is what "something other than the user drives the right edge"
    // means on a recorded source, so it turns following back on -- otherwise
    // pressing Play after a pan would advance the playhead underneath a window
    // that stayed put, and the trace would sit still while the position readout
    // climbed. Stopping leaves the window exactly where playback left it.
    if (playing_)
    {
        follow_ = true;
    }

    // The rate goes with it: a source that was told to play needs to know how
    // fast, and setRate() before the first play would otherwise be lost.
    source_->setRate(rate_);
    source_->setPlaying(playing_);
    emit changed();
}

void TimeBase::setCursor(std::optional<double> t)
{
    if (t == cursor_)
    {
        return;
    }
    cursor_ = t;
    emit cursorMoved();
}

void TimeBase::restartTimer()
{
    // Clamped above, so this can never be a 0 ms timer.
    timer_.start(std::chrono::milliseconds{1000 / render_rate_hz_});
}

}  // namespace scope

#include "scope/moc_time_base.cpp"
