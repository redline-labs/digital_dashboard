// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_TOUCH_THROTTLE_H_
#define CARPLAY_TOUCH_THROTTLE_H_

#include "helpers/rate_gate.h"

#include <chrono>
#include <optional>

// Decides which touch motion during a drag actually reaches the wire, and holds
// the one position that is waiting to.
//
// Policy and state only: no timer, no publisher, no clock, and deliberately no
// Qt -- the widget supplies all of those. That is what lets the interval
// boundaries and the deferral state machine be tested exactly, rather than
// sampled with a stopwatch against a running widget.
//
// Deliberately *not* airplay::EventQueue, despite the family resemblance. That
// one is a bounded multi-producer queue drained by a writer thread, and it rate
// limits every report including down and up -- correct there, since it is a
// guardrail against a publisher that misbehaves. This one is single threaded,
// holds at most one deferred position, and never delays a down or an up,
// because those are user-initiated transitions whose latency is felt directly.
// The two share the spacing decision (helpers::RateGate) and nothing else.
class TouchThrottle
{
  public:
    using clock = std::chrono::steady_clock;

    // Kept Qt-free on purpose; the widget converts to and from QPointF.
    struct Point
    {
        double x = 0.0;
        double y = 0.0;

        friend bool operator==(const Point& a, const Point& b)
        {
            return a.x == b.x && a.y == b.y;
        }
        friend bool operator!=(const Point& a, const Point& b) { return !(a == b); }
    };

    enum class Action
    {
        Publish,  // send this motion now
        Defer,    // held as the pending position; flush after `wait`
    };

    struct MoveDecision
    {
        Action action = Action::Publish;
        clock::duration wait{};
    };

    explicit TouchThrottle(clock::duration min_interval) : _gate(min_interval) {}

    // Motion arrived. Publishes on the leading edge, otherwise becomes the
    // pending position -- overwriting any previous one, so the newest wins and
    // stale samples never accumulate.
    MoveDecision onMove(const Point& p, clock::time_point now)
    {
        MoveDecision decision;
        const auto wait = _gate.until(now);
        if (wait == clock::duration::zero())
        {
            _pending.reset();
            _gate.mark(now);
            decision.action = Action::Publish;
            return decision;
        }
        _pending = p;
        decision.action = Action::Defer;
        decision.wait = wait;
        return decision;
    }

    bool hasPending() const { return _pending.has_value(); }

    // The deferred flush fired. Returns the position to publish, or nothing if
    // the pending position was consumed or discarded in the meantime.
    std::optional<Point> takePending(clock::time_point now)
    {
        if (!_pending.has_value())
        {
            return std::nullopt;
        }
        const Point p = *_pending;
        _pending.reset();
        _gate.mark(now);
        return p;
    }

    // A press is about to be published. Drops any pending motion: it belongs to
    // the gesture that just ended, and landing it after this down would report
    // the finger somewhere it no longer is.
    void onDown(clock::time_point now)
    {
        _pending.reset();
        _gate.mark(now);
    }

    // A release is about to be published at `p`. Returns the motion that should
    // go out immediately *before* it, if any.
    //
    // The release carries its own coordinate, so this is not about landing the
    // touch in the right place. It is about not leaving a gap in the tail of
    // the gesture, which is the part the phone measures to decide fling
    // velocity. Suppressed when the pending position is where the release
    // already is, since that would be a duplicate.
    std::optional<Point> onUp(const Point& p, clock::time_point now)
    {
        std::optional<Point> flush;
        if (_pending.has_value() && *_pending != p)
        {
            flush = *_pending;
        }
        _pending.reset();
        _gate.mark(now);
        return flush;
    }

    clock::duration minInterval() const { return _gate.minGap(); }

  private:
    helpers::RateGate _gate;
    std::optional<Point> _pending;
};

#endif  // CARPLAY_TOUCH_THROTTLE_H_
