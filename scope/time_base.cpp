#include "scope/time_base.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

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

}  // namespace

TimeBase::TimeBase(DataSource& source, QObject* parent) : QObject(parent), source_(&source)
{
    timer_.setObjectName("scope_render_timer");
    connect(&timer_, &QTimer::timeout, this, [this]() {
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
    paused_at_.reset();
    cursor_.reset();

    // A new source starts stopped. Dropping into a recording that immediately
    // began playing would move the window before anyone could look at it.
    playing_ = false;
    source_->setPlaying(false);

    // A live source has no Paused state to inherit either: its Pause button is
    // replaced by the transport controls, and leaving mode_ on Paused would
    // freeze viewEnd() at a value that was just reset.
    mode_ = Mode::Live;

    emit sourceChanged();
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
    if (clamped == window_seconds_)
    {
        return;
    }
    window_seconds_ = clamped;
    emit changed();
}

void TimeBase::setMode(Mode mode)
{
    if (mode == mode_)
    {
        return;
    }
    mode_ = mode;

    if (mode_ == Mode::Paused)
    {
        // The source keeps advancing while paused, so the frozen right edge has
        // to be captured now. Recomputing it later would un-freeze the view.
        paused_at_ = source_->now();
    }
    else
    {
        paused_at_.reset();
    }

    emit changed();
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
}

double TimeBase::viewEnd() const
{
    // Paused freezes the *view*, not the data: buffers keep filling, so
    // unpausing shows what arrived meanwhile rather than a gap.
    return paused_at_ ? *paused_at_ : source_->now();
}

// ------------------------------------------------------------------- playback

void TimeBase::seek(double t)
{
    if (!source_->caps().seekable)
    {
        return;
    }

    // Clamped to what the source actually has. A slider can be dragged past the
    // end of a recording that is still growing, and a seek beyond the data would
    // show an empty window that looks exactly like a publisher that stopped.
    const SourceCaps caps = source_->caps();
    const double clamped = std::clamp(t, caps.t_begin, caps.t_end);

    source_->seek(clamped);

    // Seeking un-freezes the view: paused_at_ is a right edge captured before
    // the move and would keep the window where it was while the data underneath
    // it changed.
    paused_at_.reset();

    emit changed();
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
