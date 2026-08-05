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

}  // namespace

TimeBase::TimeBase(DataSource& source, QObject* parent) : QObject(parent), source_(source)
{
    timer_.setObjectName("scope_render_timer");
    connect(&timer_, &QTimer::timeout, this, [this]() { emit frame(); });
    restartTimer();
}

TimeBase::~TimeBase() = default;

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
        paused_at_ = source_.now();
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
    return paused_at_ ? *paused_at_ : source_.now();
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
