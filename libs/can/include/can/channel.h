// SPDX-License-Identifier: GPL-3.0-or-later
//
// One CAN channel: the interface everything above the drivers is written
// against.
//
// Threading contract, because it is the thing that will bite otherwise:
// send() is safe to call from any thread while another thread is blocked in
// receive(). Everything else -- start, stop, set_bitrate -- must be called from
// one thread at a time and not while a receive is in flight. That is exactly
// what the bridge node needs (one reader thread per channel, senders arriving
// on zenoh callback threads) and no more, because a fully locked interface
// would put a mutex in the path of every frame for the benefit of a case
// nothing has.
#ifndef CAN_CHANNEL_H
#define CAN_CHANNEL_H

#include "can/bitrate.h"
#include "can/channel_id.h"
#include "can/error.h"

#include "helpers/can_frame.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace can
{

using Duration = std::chrono::milliseconds;

// The error state of the controller, as CAN defines it. A node walks up this
// list as errors accumulate and drops off the bus entirely at the end.
enum class BusState
{
    // Not started, or the backend cannot tell us.
    Unknown,
    // Normal. Errors are reported but the node keeps transmitting.
    ErrorActive,
    // An error counter has passed 96. Still fully on the bus, but this is the
    // first sign of a wiring or termination problem.
    ErrorWarning,
    // An error counter has passed 127. The node now waits longer before
    // retransmitting and cannot flag errors as actively.
    ErrorPassive,
    // An error counter has passed 255. The node is off the bus and transmits
    // nothing until it recovers.
    BusOff,
    Stopped,
};

const char* to_string(BusState state);

struct Statistics
{
    uint64_t rxFrames { 0 };
    uint64_t txFrames { 0 };
    uint64_t rxBytes { 0 };
    uint64_t txBytes { 0 };
    // Frames the backend saw but could not hand over, because the queue was
    // full or the adapter reported its own overrun. Non-zero means frames were
    // lost, which for a bus being logged is the difference between a trace you
    // can trust and one you cannot.
    uint64_t rxDropped { 0 };
    uint64_t txDropped { 0 };
    // Error frames received, and how many times the controller has gone
    // bus-off.
    uint64_t errorFrames { 0 };
    uint64_t busOffCount { 0 };

    uint8_t rxErrorCounter { 0 };
    uint8_t txErrorCounter { 0 };
    BusState state { BusState::Unknown };
};

class Channel
{
public:
    virtual ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    virtual const ChannelId& id() const = 0;
    // What this channel is, for a log line: "PCAN-USB Pro FD channel 1".
    virtual const std::string& description() const = 0;

    // --- configuration ------------------------------------------------------
    //
    // Changing the bit rate means taking the controller out of its normal
    // mode, so a channel that is running is stopped and restarted around it.
    // A backend that cannot do this without privileges it does not have
    // returns PermissionDenied rather than half-applying.
    virtual Result<void> set_bitrate(const Bitrate& bitrate) = 0;
    virtual Bitrate bitrate() const = 0;
    virtual bool supports_fd() const = 0;

    // Receive without transmitting -- no acknowledgements, no error frames.
    // The safe way to attach to a bus you do not own.
    virtual Result<void> set_listen_only(bool listenOnly) = 0;
    virtual bool listen_only() const = 0;

    // --- lifecycle ----------------------------------------------------------
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
    virtual bool running() const = 0;

    // --- traffic ------------------------------------------------------------
    //
    // Safe to call from any thread, including while another is in receive().
    virtual Result<void> send(const helpers::CanFrame& frame) = 0;

    // Fills as much of `out` as is available, waiting up to `timeout` for the
    // first frame and returning promptly once it has something. Returns the
    // number of frames written, which is zero on timeout -- a quiet bus is not
    // an error.
    virtual Result<size_t> receive(std::span<helpers::CanFrame> out, Duration timeout) = 0;

    virtual Statistics statistics() const = 0;

protected:
    Channel() = default;
};

} // namespace can

#endif // CAN_CHANNEL_H
