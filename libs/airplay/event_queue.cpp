// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay/event_queue.h"

namespace airplay
{

bool EventQueue::pushTouch(const TouchReport& report)
{
    // Collapse a run of motion into its newest position. However fast reports
    // arrive, consecutive moves occupy one slot, so a flood costs a memory
    // write rather than an unbounded queue and a burst of link traffic -- and
    // what the phone eventually sees is where the finger actually is.
    if (report.coalescable && !_queue.empty() && _queue.back().coalescable)
    {
        _queue.back() = report;
        return true;
    }

    if (_queue.size() >= kMaxQueued)
    {
        ++_dropped;
        return false;
    }

    _queue.push_back(report);
    return true;
}

void EventQueue::clear()
{
    _queue.clear();
    _keyframe_pending = false;
    // The next session starts fresh: its first touch has nothing to be spaced
    // against, and should not inherit the timing of the one that just ended.
    _touch_gate.reset();
}

EventQueue::Next EventQueue::take(std::chrono::steady_clock::time_point now)
{
    Next next;

    // Keyframe requests go first and are never rate limited.
    if (_keyframe_pending)
    {
        _keyframe_pending = false;
        next.action = Action::SendKeyframe;
        return next;
    }

    if (_queue.empty())
    {
        return next;  // Idle
    }

    // Rate limit touch, but wait rather than drop: the delay costs resolution,
    // not accuracy, because moves arriving meanwhile coalesce onto the tail.
    // The first report of a session has nothing to be spaced against and goes
    // straight out, so a tap never pays the gap.
    if (const auto wait = _touch_gate.until(now); wait > std::chrono::steady_clock::duration::zero())
    {
        next.action = Action::WaitTouch;
        next.wait = wait;
        return next;
    }

    next.action = Action::SendTouch;
    next.touch = _queue.front();
    _queue.pop_front();
    _touch_gate.mark(now);
    return next;
}

}  // namespace airplay
