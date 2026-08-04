// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one thing every CANopen service needs and none of them should own: a way
// to put a frame on the wire, hear the frames that come back, and know what
// time it is.
//
// Time is part of the interface rather than something the protocol code reads
// from the system clock, and that is the whole reason the reconfiguration
// sequence can be tested on a laptop. A bus backed by a simulated keypad
// advances its own clock only when asked, so a one-second SDO timeout costs
// nothing to exercise and a test that waits for a boot-up heartbeat is
// deterministic rather than a race against a real device.
#ifndef CANOPEN_BUS_H
#define CANOPEN_BUS_H

#include "helpers/can_frame.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace canopen
{

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::milliseconds;

class Bus
{
public:
    using FrameHandler = std::function<void(const helpers::CanFrame&)>;

    virtual ~Bus();

    // Put a frame on the wire. Fire and forget: every protocol on top of this
    // matches its own responses.
    virtual void send(const helpers::CanFrame& frame) = 0;

    // Let the transport deliver whatever has arrived, spending at most
    // `budget` doing so. Implementations may return early when there is
    // nothing left to deliver, and must advance now() by the time they
    // actually consumed.
    virtual void poll(Duration budget) = 0;

    virtual Clock::time_point now() const = 0;

    // Every service that needs to see frames registers here: an SDO client
    // watching for 0x580+id, a heartbeat monitor watching 0x700+id, a PDO
    // decoder watching the PDO COB-IDs. Handlers are called in registration
    // order and none of them can stop the others from being called -- a frame
    // is not "consumed".
    void subscribe(FrameHandler handler);

protected:
    // Implementations call this for each frame received.
    void deliver(const helpers::CanFrame& frame);

private:
    std::vector<FrameHandler> handlers_;
};

// Waits until `predicate` is true or `timeout` elapses, polling the bus in
// between. Returns whether the predicate came true.
//
// Every request/response exchange in this library is shaped this way, and
// having one implementation of it means one place decides how finely to poll.
bool wait_until(Bus& bus, Duration timeout, const std::function<bool()>& predicate);

} // namespace canopen

#endif // CANOPEN_BUS_H
