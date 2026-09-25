// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef AIRPLAY_EVENT_QUEUE_H_
#define AIRPLAY_EVENT_QUEUE_H_

#include "helpers/rate_gate.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace airplay
{

// Outbound work for the AirPlay event channel: input reports headed for the
// phone, and requests for a fresh keyframe.
//
// This is policy and data only -- deliberately not thread safe and with no
// clock of its own. Receiver owns a mutex, a condition variable and a real
// steady_clock around it; keeping those out here is what lets the interesting
// decisions (what coalesces, what jumps the queue, what gets dropped) be tested
// without threads, sockets, timing, or a phone.
//
// Three kinds of work with different rules:
//
//  - Touch is strictly ordered, because a gesture is a sequence and reordering
//    it is meaningless. Consecutive moves coalesce onto the tail rather than
//    accumulating, which is what bounds this under a publisher sending faster
//    than the link drains. Down and up never coalesce, so the gesture's shape
//    survives -- collapsing a down into a following move would relocate the
//    press and turn a drag into a tap somewhere else. With two fingers the
//    same rule reads: a report coalesces only onto one with the same fingers
//    down, so a second finger landing or lifting is never merged away.
//  - A control command -- a knob turn, a media key, a phone key, a Siri
//    request -- is queued in its own strictly ordered FIFO, and never
//    coalesced: each one is a distinct press or detent, and merging two of them
//    loses one. It is handed out ahead of queued touch and is not rate limited.
//    There is no ordering relationship to lose (they are separate HID devices),
//    and a button that waits behind a backlog of finger movement is a button
//    that feels broken.
//  - A keyframe request is a flag, not a queue entry, because it is idempotent:
//    several pending requests are one request. It also has no ordering
//    relationship to touch, so it is handed out ahead of queued touch and is
//    never rate limited. That matters -- the keyframe request is what recovers
//    a black screen for a renderer that joined late.
class EventQueue
{
  public:
    // Contact slots in a touch report. Matches hid::kTouchContacts, which
    // event_channel.cpp asserts; not included from there because the queue has
    // no business knowing HID.
    static constexpr int kTouchSlots = 2;

    // One slot of a touch report. `down` is the wire-level contact bit, set
    // for both a press and a move; a slot with no finger is left default.
    struct TouchContact
    {
        float x = 0.0f;
        float y = 0.0f;
        bool down = false;
    };

    // The whole digitizer state at one instant: every slot, because that is
    // what one HID report carries. One finger is simply slot 0 with slot 1
    // empty; there is no separate single-touch form.
    //
    // `coalescable` is the queue's to set, not the caller's: pushTouch marks a
    // report motion only when no finger landed or lifted since the last one.
    struct TouchReport
    {
        std::array<TouchContact, kTouchSlots> contacts{};
        bool coalescable = false;

        // Bit i set when slot i is down.
        unsigned downMask() const
        {
            unsigned mask = 0;
            for (size_t i = 0; i < contacts.size(); ++i)
            {
                mask |= contacts[i].down ? (1u << i) : 0u;
            }
            return mask;
        }
    };

    // One already-encoded event-channel command body. Encoded by the caller,
    // because the queue has no business knowing what a knob is -- it only
    // orders and paces.
    using ControlCommand = std::vector<uint8_t>;

    enum class Action
    {
        Idle,          // nothing to do
        SendKeyframe,  // send a keyframe request now
        SendControl,   // send `control` now
        SendTouch,     // send `touch` now
        WaitTouch,     // touch is ready but rate limited; retry after `wait`
    };

    struct Next
    {
        Action action = Action::Idle;
        TouchReport touch{};
        ControlCommand control;
        std::chrono::steady_clock::duration wait{};
    };

    // Past this many queued reports, further ones are dropped. Only reachable
    // when the link has stalled and down/up pairs (which cannot coalesce) keep
    // arriving -- moves alone can never grow the queue.
    static constexpr size_t kMaxQueued = 64;

    // Minimum spacing between touch reports leaving for the phone. A guardrail
    // rather than a throttle: the dashboard widget already paces itself to
    // 60 Hz, and this sits well above that so it never engages in normal
    // operation. It exists to bound a publisher that ignores its own limit.
    static constexpr std::chrono::milliseconds kMinTouchGap{8};  // 125 Hz

    // Returns false if the report was dropped because the queue is full.
    //
    // The sender gives no phase -- down, move or up -- because the phase is a
    // property of the sequence, which only the queue sees: a report is motion
    // when the same fingers are down as in the last one pushed, and otherwise a
    // finger landed or lifted. For one finger that is exactly down, move, up.
    bool pushTouch(TouchReport report);

    // Same bound, counted against the same drop total. Nothing coalesces here,
    // so a stalled link fills this queue -- but only at the rate a human can
    // press buttons, which is why it needs no gate of its own.
    bool pushControl(ControlCommand command);

    void requestKeyframe() { _keyframe_pending = true; }

    // Drops everything pending. Used when the event channel closes: what is
    // queued belongs to the session that just ended, most likely a touch whose
    // matching release never got sent, and replaying it into the next session
    // would inject a phantom contact.
    void clear();

    // True when take() would return something other than Idle, ignoring the
    // rate limit. Suitable as a condition-variable predicate.
    bool hasWork() const
    {
        return _keyframe_pending || !_control_queue.empty() || !_queue.empty();
    }

    // Decides what to do at `now`, and consumes whatever it hands out. A
    // WaitTouch result consumes nothing: the caller waits and asks again, which
    // is what lets a move arriving during the wait coalesce, and lets a
    // keyframe request that lands during the wait overtake the queued touch.
    Next take(std::chrono::steady_clock::time_point now);

    size_t size() const { return _queue.size(); }
    size_t controlSize() const { return _control_queue.size(); }
    bool keyframePending() const { return _keyframe_pending; }
    uint64_t dropped() const { return _dropped; }

  private:
    std::deque<TouchReport> _queue;
    std::deque<ControlCommand> _control_queue;
    bool _keyframe_pending = false;
    // Down mask of the last touch report pushed, dropped or not: what
    // pushTouch compares against. Zero is "no finger down", which is what
    // a new session starts from.
    unsigned _last_touch_mask = 0;
    helpers::RateGate _touch_gate{kMinTouchGap};
    uint64_t _dropped = 0;
};

}  // namespace airplay

#endif  // AIRPLAY_EVENT_QUEUE_H_
