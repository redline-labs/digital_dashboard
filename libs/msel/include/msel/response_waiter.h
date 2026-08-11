// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hand-off between the thread that sends a configuration command and the
// thread that decodes the relay's answer.
//
// It lives here rather than in the node because it is the part with the
// interesting failure modes and no zenoh in it, which is the same reason the
// protocol and the decoder are here: a threading rendezvous that only exists
// inside a service callback can be reasoned about but not tested.
//
// WHY THERE IS A RENDEZVOUS AT ALL. The relay answers a configuration command
// on its base status identifier, seconds later, on whatever thread CAN frames
// arrive on -- there is no request/response pairing on the wire and no
// correlation id to match. So the sending thread arms this, sends, and waits;
// the receiving thread delivers whatever answer turns up.
//
// The device makes silence the ordinary case: a command is ignored outright
// unless a human is holding the external kill switch as the frame arrives, and
// an ignored command is not answered at all. So a timeout here is a normal
// outcome to be reported, not an error to be raised.
#ifndef MSEL_RESPONSE_WAITER_H
#define MSEL_RESPONSE_WAITER_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

#include "msel/protocol.h"

namespace msel
{

class ResponseWaiter
{
  public:
    // Start listening, before the command frame goes out.
    //
    // Arming FIRST is what closes the race: on a fast bus the answer can be
    // decoded before the sending thread gets back from its publish, and a
    // waiter armed afterwards would wait out its whole timeout for an answer
    // that had already arrived.
    void arm();

    // From the receive thread, once per decoded response.
    //
    // A response delivered while nothing is armed is DROPPED, deliberately.
    // Keeping it would mean the next command inherited an answer to the
    // previous one -- and since these arrive late by nature, that is not a
    // corner case, it is what happens whenever someone gives up on a command
    // and tries another. Reporting the wrong command as accepted is worse than
    // reporting nothing.
    void deliver(ConfigResponse response);

    // Blocks until an answer arrives or `timeout` elapses, then disarms.
    // nullopt means the relay said nothing, which usually means the external
    // kill switch was not held.
    //
    // Never call this holding a lock the receive thread needs, or the answer
    // cannot arrive and every command times out.
    std::optional<ConfigResponse> wait(std::chrono::milliseconds timeout);

    // Give up listening without waiting -- the frame was never sent, so no
    // answer is coming.
    void disarm();

    // Test seam, and a cheap assertion for the node: nothing should be armed
    // between commands.
    bool armed() const;

  private:
    mutable std::mutex mMutex;
    std::condition_variable mSignal;
    bool mArmed { false };
    std::optional<ConfigResponse> mResponse;
};

} // namespace msel

#endif // MSEL_RESPONSE_WAITER_H
