// SPDX-License-Identifier: GPL-3.0-or-later
// Ordering, coalescing, rate limiting and drop rules for the AirPlay event
// channel queue. No threads, no sockets, no clock, no phone: time is injected,
// so every case here is deterministic.
#include "airplay/event_queue.h"

#include <spdlog/spdlog.h>

#include <string>

namespace
{

using airplay::EventQueue;
using Action = airplay::EventQueue::Action;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

EventQueue::TouchReport down(float x, float y)
{
    return {x, y, true, false};
}

EventQueue::TouchReport move(float x, float y)
{
    return {x, y, true, true};
}

EventQueue::TouchReport up(float x, float y)
{
    return {x, y, false, false};
}

// A fixed origin plus offsets, so every test states its own timing.
const std::chrono::steady_clock::time_point kT0{};

std::chrono::steady_clock::time_point at(int ms)
{
    return kT0 + std::chrono::milliseconds(ms);
}

// Far enough past the last send that the rate limit is satisfied.
std::chrono::steady_clock::time_point later(int ms)
{
    return at(ms) + EventQueue::kMinTouchGap;
}

void testCoalescingKeepsNewestPosition()
{
    EventQueue q;
    q.pushTouch(move(1, 1));
    q.pushTouch(move(2, 2));
    q.pushTouch(move(3, 3));

    expect(q.size() == 1, "three consecutive moves occupy one slot");

    const auto next = q.take(later(0));
    expect(next.action == Action::SendTouch, "coalesced move is sent");
    expect(next.touch.x == 3.0f && next.touch.y == 3.0f,
           "coalescing keeps the newest position, not the oldest");
}

void testCoalescingNeverCrossesDownOrUp()
{
    // The regression: collapsing a down into a following move would relocate
    // the press, turning a drag from (1,1) into a tap at (9,9).
    EventQueue q;
    q.pushTouch(down(1, 1));
    q.pushTouch(move(9, 9));
    expect(q.size() == 2, "a move does not coalesce into a preceding down");

    auto next = q.take(later(0));
    expect(next.action == Action::SendTouch && next.touch.x == 1.0f,
           "the down is delivered first, at the position it was pressed");
    next = q.take(later(100));
    expect(next.action == Action::SendTouch && next.touch.x == 9.0f,
           "the move follows, in order");

    // Same on the other side: an up must not absorb, or be absorbed by, motion.
    EventQueue q2;
    q2.pushTouch(move(4, 4));
    q2.pushTouch(up(5, 5));
    q2.pushTouch(move(6, 6));
    expect(q2.size() == 3, "up neither coalesces with the move before it nor the one after");
}

void testGestureOrderIsPreserved()
{
    EventQueue q;
    q.pushTouch(down(0, 0));
    q.pushTouch(move(1, 0));
    q.pushTouch(move(2, 0));  // coalesces onto the previous move
    q.pushTouch(up(3, 0));

    expect(q.size() == 3, "down, coalesced move, up");

    int t = 0;
    auto a = q.take(later(t += 100));
    auto b = q.take(later(t += 100));
    auto c = q.take(later(t += 100));

    expect(a.touch.down && a.touch.x == 0.0f, "down first");
    expect(b.touch.down && b.touch.x == 2.0f, "then the newest move");
    expect(!c.touch.down && c.touch.x == 3.0f, "then up, with the contact bit cleared");
    expect(q.take(later(t += 100)).action == Action::Idle, "and then nothing");
}

void testKeyframeOvertakesTouchAndIsNotRateLimited()
{
    EventQueue q;
    for (int i = 0; i < 10; ++i)
    {
        q.pushTouch(down(static_cast<float>(i), 0));  // non-coalescable, so they stack up
    }
    q.requestKeyframe();

    // Even with ten touch reports already queued, the keyframe goes first.
    const auto next = q.take(at(0));
    expect(next.action == Action::SendKeyframe, "a keyframe request overtakes queued touch");
    expect(q.size() == 10, "and consumes no touch doing so");

    // And it is exempt from the touch rate limit: back-to-back at the same
    // instant, where touch would be told to wait.
    q.requestKeyframe();
    expect(q.take(at(0)).action == Action::SendKeyframe,
           "a keyframe request is never rate limited");
}

void testKeyframeRequestsCollapse()
{
    EventQueue q;
    q.requestKeyframe();
    q.requestKeyframe();
    q.requestKeyframe();

    expect(q.take(at(0)).action == Action::SendKeyframe, "the collapsed request is delivered");
    expect(!q.keyframePending(), "three pending requests are one request, not three");
    expect(q.take(at(0)).action == Action::Idle, "nothing is left over");
}

void testTouchIsRateLimitedByWaitingNotDropping()
{
    EventQueue q;
    q.pushTouch(move(1, 1));
    const auto first = q.take(at(0));
    expect(first.action == Action::SendTouch, "the first report goes immediately");

    // A second report inside the gap is told to wait, and crucially is *not*
    // consumed -- so a newer position can still coalesce onto it.
    q.pushTouch(move(2, 2));
    const auto blocked = q.take(at(1));
    expect(blocked.action == Action::WaitTouch, "a report inside the gap waits");
    expect(blocked.wait > std::chrono::steady_clock::duration::zero(),
           "and says how long to wait for");
    expect(q.size() == 1, "waiting consumes nothing");

    // That is what makes the wait useful: motion during it coalesces, so what
    // finally goes out is where the finger is now, not where it was.
    q.pushTouch(move(7, 7));
    const auto after = q.take(at(0) + EventQueue::kMinTouchGap);
    expect(after.action == Action::SendTouch && after.touch.x == 7.0f,
           "the report sent after the wait is the newest position");
}

void testKeyframeOvertakesDuringATouchWait()
{
    // The interaction that motivated all of this: a keyframe request must not
    // be stuck behind touch that is itself waiting on the rate limit.
    EventQueue q;
    q.pushTouch(move(1, 1));
    q.take(at(0));  // consume, arming the rate limit

    q.pushTouch(move(2, 2));
    expect(q.take(at(1)).action == Action::WaitTouch, "touch is rate limited");

    q.requestKeyframe();
    expect(q.take(at(1)).action == Action::SendKeyframe,
           "a keyframe landing during a touch wait is sent without waiting");
}

void testFullQueueDropsRatherThanGrows()
{
    EventQueue q;
    // Only non-coalescable reports can accumulate; moves would collapse.
    for (size_t i = 0; i < EventQueue::kMaxQueued; ++i)
    {
        expect(q.pushTouch(down(1, 1)), "accepted while under the cap");
    }
    expect(q.size() == EventQueue::kMaxQueued, "queue fills to the cap");

    expect(!q.pushTouch(down(2, 2)), "a push past the cap is rejected");
    expect(q.size() == EventQueue::kMaxQueued, "and the queue does not grow");
    expect(q.dropped() == 1, "the drop is counted so it can be reported");

    // Moves still coalesce onto a full queue only if the tail is a move; here
    // it is not, so they are dropped too rather than growing it.
    expect(!q.pushTouch(move(3, 3)), "a move onto a full queue with a non-move tail is dropped");
    expect(q.dropped() == 2, "and counted");
}

void testFloodOfMotionNeverGrowsTheQueue()
{
    // The guardrail, stated directly: an unbounded flood of motion costs one
    // slot, no drops, and the phone still ends up with the right position.
    EventQueue q;
    for (int i = 0; i < 100000; ++i)
    {
        q.pushTouch(move(static_cast<float>(i), 0));
    }
    expect(q.size() == 1, "100k moves occupy one slot");
    expect(q.dropped() == 0, "and none are dropped");
    expect(q.take(later(0)).touch.x == 99999.0f, "the newest position survives");
}

void testFirstReportIsNeverRateLimited()
{
    // A fresh queue has nothing to space the first report against, so it must
    // go straight out. Guarding the sloppy version of this: leaving the "last
    // sent" timestamp at its default makes "never sent" indistinguishable from
    // "sent at the clock epoch", which real steady_clock values are far enough
    // past to hide -- until something resets or a different clock origin shows
    // up. Asserted at the epoch precisely because that is the value that breaks.
    EventQueue q;
    q.pushTouch(down(1, 1));
    expect(q.take(kT0).action == Action::SendTouch,
           "the first report of a session is not rate limited, even at the clock epoch");

    // Same again after a channel close: the next session should not inherit the
    // timing of the one that ended.
    q.pushTouch(move(2, 2));
    q.take(later(0));
    q.clear();
    q.pushTouch(down(3, 3));
    expect(q.take(kT0).action == Action::SendTouch,
           "the first report after a clear is not rate limited either");
}

void testClearDropsAnOrphanedGesture()
{
    // On channel close: a touch whose release never arrived must not be
    // replayed into the next session as a phantom contact.
    EventQueue q;
    q.pushTouch(down(1, 1));
    q.pushTouch(move(2, 2));
    q.requestKeyframe();

    q.clear();
    expect(q.size() == 0, "clear drops queued touch");
    expect(!q.keyframePending(), "and any pending keyframe request");
    expect(!q.hasWork(), "leaving no work");
    expect(q.take(at(0)).action == Action::Idle, "and nothing to take");
}

void testControlOvertakesTouchButNotKeyframes()
{
    // A button press should not wait behind a backlog of finger movement, and
    // must not be rate limited by the touch gate -- but a keyframe request
    // still comes first, because that is what recovers a black screen.
    EventQueue q;
    q.pushTouch(down(1, 1));
    q.pushControl({0xAA});
    q.requestKeyframe();

    expect(q.take(at(0)).action == Action::SendKeyframe, "keyframes still go first");

    const auto control = q.take(at(0));
    expect(control.action == Action::SendControl, "control overtakes queued touch");
    expect(control.control == EventQueue::ControlCommand({0xAA}), "and carries its body");

    // The touch that was queued first is still there, in order.
    expect(q.take(later(0)).action == Action::SendTouch, "the touch is not lost");
}

void testControlIsOrderedAndNotCoalesced()
{
    // Two presses of the same button are two presses. Nothing here may merge
    // them the way consecutive moves merge.
    EventQueue q;
    q.pushControl({1});
    q.pushControl({1});
    q.pushControl({2});
    expect(q.controlSize() == 3, "identical control commands both survive");

    expect(q.take(at(0)).control == EventQueue::ControlCommand({1}), "first out is first in");
    expect(q.take(at(0)).control == EventQueue::ControlCommand({1}), "then its duplicate");
    expect(q.take(at(0)).control == EventQueue::ControlCommand({2}), "then the next");
    expect(!q.hasWork(), "and the queue drains");
}

void testControlQueueIsBoundedAndClearedWithTheSession()
{
    EventQueue q;
    for (size_t i = 0; i < EventQueue::kMaxQueued + 5; ++i)
    {
        q.pushControl({static_cast<uint8_t>(i)});
    }
    expect(q.controlSize() == EventQueue::kMaxQueued, "the control queue is bounded");
    expect(q.dropped() == 5, "and the overflow is counted as dropped");

    // A held button is the control equivalent of a touch with no release.
    q.clear();
    expect(q.controlSize() == 0, "clear drops queued control commands");
    expect(!q.hasWork(), "leaving no work");
}

void testHasWorkTracksTakeableWork()
{
    EventQueue q;
    expect(!q.hasWork(), "an empty queue has no work");

    q.pushTouch(move(1, 1));
    expect(q.hasWork(), "queued touch is work");
    q.take(later(0));
    expect(!q.hasWork(), "and is not once taken");

    q.requestKeyframe();
    expect(q.hasWork(), "a keyframe request is work");
    q.take(at(0));
    expect(!q.hasWork(), "and is not once taken");

    // hasWork ignores the rate limit deliberately: it is the condition variable
    // predicate, and the waiting is the caller's job.
    q.pushTouch(move(1, 1));
    q.take(later(0));
    q.pushTouch(move(2, 2));
    expect(q.hasWork(), "rate-limited touch still counts as work to wake up for");
}

}  // namespace

int main()
{
    testCoalescingKeepsNewestPosition();
    testCoalescingNeverCrossesDownOrUp();
    testGestureOrderIsPreserved();
    testKeyframeOvertakesTouchAndIsNotRateLimited();
    testKeyframeRequestsCollapse();
    testTouchIsRateLimitedByWaitingNotDropping();
    testKeyframeOvertakesDuringATouchWait();
    testFirstReportIsNeverRateLimited();
    testFullQueueDropsRatherThanGrows();
    testFloodOfMotionNeverGrowsTheQueue();
    testClearDropsAnOrphanedGesture();
    testControlOvertakesTouchButNotKeyframes();
    testControlIsOrderedAndNotCoalesced();
    testControlQueueIsBoundedAndClearedWithTheSession();
    testHasWorkTracksTakeableWork();

    if (failures == 0)
    {
        SPDLOG_INFO("all event queue tests passed");
    }
    return failures == 0 ? 0 : 1;
}
