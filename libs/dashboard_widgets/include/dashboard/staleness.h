// SPDX-License-Identifier: GPL-3.0-or-later
//
// When a reading has stopped arriving.
//
// Every gauge used to hold its last value forever, so a dead sensor and a
// steady one looked identical -- which is the wrong failure for a vehicle
// display. A binding says how long a gap means "no data" (`stale_after_ms` in
// the widget's config, 0 to never), ExpressionSubscription runs this tracker on
// the shared delivery tick, and the widget is told on each edge so it can draw
// the difference.
//
// No Qt and no clock reads here: the caller passes the time, which is what
// makes every boundary testable.
#ifndef DASHBOARD_STALENESS_H_
#define DASHBOARD_STALENESS_H_

#include <chrono>
#include <optional>

namespace dashboard
{

class StalenessTracker
{
  public:
    using clock = std::chrono::steady_clock;

    enum class Edge
    {
        none,
        became_stale,
        became_fresh,
    };

    // `armed_at` starts the clock: a binding that never receives anything goes
    // stale `timeout` after construction rather than looking fine forever.
    StalenessTracker(std::chrono::milliseconds timeout, clock::time_point armed_at)
        : timeout_(timeout)
        , last_(armed_at)
    {
    }

    bool enabled() const { return timeout_.count() > 0; }
    bool isStale() const { return stale_; }

    Edge onSample(clock::time_point at)
    {
        last_ = at;
        if (!enabled() || !stale_)
        {
            return Edge::none;
        }
        stale_ = false;
        return Edge::became_fresh;
    }

    // The earliest time poll() could report became_stale, so a caller can
    // sleep until then instead of polling. None while disabled or stale.
    std::optional<clock::time_point> deadline() const
    {
        if (!enabled() || stale_)
        {
            return std::nullopt;
        }
        return last_ + timeout_;
    }

    Edge poll(clock::time_point now)
    {
        if (!enabled() || stale_ || now - last_ < timeout_)
        {
            return Edge::none;
        }
        stale_ = true;
        return Edge::became_stale;
    }

  private:
    std::chrono::milliseconds timeout_;
    clock::time_point last_;
    bool stale_ = false;
};

// Whether staleness is reported at all in this process.
//
// The editor previews a layout with no bus behind it, so every binding there
// would go stale and every widget would draw its no-data look. It turns this on
// once in main(); the dashboard never does.
namespace staleness
{

inline bool& suppressedFlag()
{
    static bool suppressed = false;
    return suppressed;
}

inline void setSuppressed(bool suppressed) { suppressedFlag() = suppressed; }
inline bool suppressed() { return suppressedFlag(); }

}  // namespace staleness

}  // namespace dashboard

#endif  // DASHBOARD_STALENESS_H_
