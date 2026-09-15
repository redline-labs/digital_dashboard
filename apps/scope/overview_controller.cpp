#include "scope/overview_controller.h"

#include "scope/live_zenoh_source.h"
#include "scope/overview_strip.h"
#include "scope/scope_recorder.h"
#include "scope/scope_window.h"
#include "scope/time_base.h"

#include <QDateTime>

#include <algorithm>

namespace scope
{

namespace
{

// How often the overview's histogram may be recomputed. Fast enough that a
// growing capture visibly grows, slow enough that walking millions of retained
// messages under the capture's mutex does not stall the zenoh RX thread that
// needs the same lock to push. See refreshDensity().
constexpr std::int64_t kDensityIntervalMs = 500;

}  // namespace

void OverviewController::updateOverview()
{
    OverviewStrip* overview = window_.overview_;
    if (overview == nullptr)
    {
        return;
    }

    const SourceCaps caps = window_.source_->caps();
    const double now = window_.source_->now();

    // The extent, and the two source kinds answer it differently for a real
    // reason. A recording has a beginning; a bus does not, so the honest bound
    // for a live source is how far back the panels' buffers still reach.
    double extent_begin = 0.0;
    double extent_end = 0.0;
    if (caps.seekable)
    {
        extent_begin = caps.t_begin;
        extent_end = caps.t_end;
    }
    else
    {
        // Clamped at the source's own epoch: a live source's clock starts at
        // zero and there is nothing before it. Without the clamp a freshly
        // started session shows five minutes of strip for thirty seconds of
        // data, and the histogram -- which can only be counted from the epoch
        // forward -- ends up drawn against a range it was never counted over.
        extent_begin = std::max(now - window_.history_seconds_, 0.0);
        extent_end = now;
    }
    overview->setExtent(extent_begin, extent_end);

    // What the panels can actually draw. On a recording it is the whole thing;
    // on a live source it is the same as the extent today, and will narrow once
    // the strip can show the capture behind it.
    overview->setRetained(std::max(extent_begin, now - window_.history_seconds_), extent_end);

    overview->setView(window_.time_base_->viewBegin(), window_.time_base_->viewEnd());
    overview->setTimeCursor(window_.time_base_->cursor());

    // Only a seekable source has a position to mark. A live source's "now" is
    // the right edge of the extent, where a playhead would be noise.
    overview->setPlayhead(caps.seekable ? std::optional<double>(now) : std::nullopt);

    if (window_.recorder_ != nullptr)
    {
        overview->setEvicted(window_.recorder_->buffer().evicted());
    }

    refreshDensity();
}

bool OverviewController::densityFor(double begin, double end, std::size_t buckets,
                                    std::vector<std::uint32_t>& out)
{
    if (window_.source_->density(begin, end, buckets, out))
    {
        return true;
    }

    // A live source keeps no history of its own -- only the buffers the panels
    // hold -- so it declines. The recorder has been capturing the whole bus
    // since the window opened, and THAT is the honest picture of where the
    // traffic is. The window is the only thing holding both, which is why the
    // reconciliation lives here rather than behind the DataSource seam.
    const auto* live = dynamic_cast<const LiveZenohSource*>(window_.source_.get());
    if (live == nullptr || window_.recorder_ == nullptr)
    {
        out.clear();
        return false;
    }

    // Seconds on the live source's steady clock -> UNIX nanoseconds, through
    // the wall-clock instant sampled beside its steady epoch. Without that pair
    // the two clocks have no common origin at all.
    const std::uint64_t epoch = live->epochWallNanos();
    const auto to_nanos = [epoch](double t) {
        return epoch + static_cast<std::uint64_t>(std::max(t, 0.0) * 1e9);
    };
    window_.recorder_->buffer().density(to_nanos(begin), to_nanos(end), buckets, out);
    return true;
}

std::pair<double, double> OverviewController::densityRange() const
{
    const SourceCaps caps = window_.source_->caps();
    if (caps.seekable)
    {
        return {caps.t_begin, caps.t_end};
    }

    // Floored at the source's epoch for a LIVE source, and only here: the
    // capture can only be counted from the moment it started, so asking for
    // counts before that would label the histogram with a range it was never
    // counted over. The view itself is deliberately not floored -- see
    // TimeBase::availableRange().
    const double now = window_.source_->now();
    return {std::max(now - window_.history_seconds_, 0.0), now};
}

void OverviewController::refreshDensity()
{
    OverviewStrip* overview = window_.overview_;
    if (overview == nullptr)
    {
        return;
    }

    // One bucket per pixel of the strip. More would be invisible and cost a
    // longer walk under the capture's mutex; fewer would throw away detail the
    // widget has room to show.
    const int buckets = std::max(overview->width(), 1);

    const auto [begin, end] = densityRange();

    const std::int64_t now_ms = QDateTime::currentMSecsSinceEpoch();

    // Two triggers, and each covers what the other cannot.
    //
    // `moved` catches a RESIZE, where the cached counts have the wrong number
    // of buckets and must be redrawn at once. A source swap forces a recompute
    // through forceRecompute().
    //
    // `due` covers a capture growing under the strip. The buffer's revision is
    // useless as a cache key here (it bumps on every push, thousands a second),
    // so the clock is what bounds the work. A bag recomputes on this tick too
    // and costs nothing: its answer comes from a handful of part records.
    //
    // DELIBERATELY NOT comparing begin/end: on a live source `end` is now(),
    // which is fresh every call, so a range comparison is always "moved" and
    // the throttle never fires -- which put the O(retained) walk under the RX
    // thread's mutex at the render rate, the exact thing the 500 ms interval
    // exists to prevent. The strip drawing a histogram up to half a second old
    // behind a moving edge is the accepted trade.
    const bool moved = buckets != buckets_;
    const bool due = now_ms - computed_at_ms_ >= kDensityIntervalMs;

    if (!moved && !due)
    {
        return;
    }

    if (!densityFor(begin, end, static_cast<std::size_t>(buckets), density_))
    {
        // A source that cannot answer cheaply says so, and the strip draws a
        // plain band. Clearing rather than keeping the last answer matters on a
        // swap: a bag's histogram left behind by going online would describe a
        // recording that is no longer on screen.
        overview->setDensity({}, begin, end);
        buckets_ = buckets;
        computed_at_ms_ = now_ms;
        return;
    }

    overview->setDensity(density_, begin, end);
    buckets_ = buckets;
    computed_at_ms_ = now_ms;
}

}  // namespace scope
