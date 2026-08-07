// SPDX-License-Identifier: GPL-3.0-or-later
//
// A canopen::Bus over zenoh: frames out on `vehicle/can0/tx`, frames in on
// `vehicle/can0/rx`.
//
// `nodes/can_bridge` is what terminates the tx key: it opens a real adapter --
// or a recorded trace, via `trc:` -- and puts whatever arrives on that topic
// onto the bus. This class is the half that does not depend on which adapter
// that turns out to be: it is what the reconfiguration tool selects with
// `--transport zenoh`.
//
// Still untested against hardware. The stub transport is the one with tests
// behind it.
//
// Threading: zenoh delivers on its own thread, and the CANopen protocol code is
// single-threaded and synchronous. Received frames are therefore queued and
// handed over inside poll(), on the caller's thread, so a handler never runs
// concurrently with the code that is waiting for it.
#ifndef CANOPEN_ZENOH_BUS_H
#define CANOPEN_ZENOH_BUS_H

#include "canopen/bus.h"

#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace canopen
{

class ZenohBus : public Bus
{
public:
    ZenohBus(std::string txKey = "vehicle/can0/tx", std::string rxKey = "vehicle/can0/rx");
    ~ZenohBus() override;

    ZenohBus(const ZenohBus&) = delete;
    ZenohBus& operator=(const ZenohBus&) = delete;

    void send(const helpers::CanFrame& frame) override;
    void poll(Duration budget) override;
    Clock::time_point now() const override;

    // False when the zenoh session could not be opened, in which case nothing
    // will be sent or received and every exchange will time out. Worth
    // checking before blaming the device.
    bool is_valid() const;

    const std::string& tx_key() const { return txKey_; }
    const std::string& rx_key() const { return rxKey_; }

    // How many frames have been dropped because the receive queue was full.
    // Non-zero means the caller is not polling often enough.
    uint64_t dropped() const;

private:
    // Bounded so a caller that stops polling cannot grow the queue without
    // limit. An SDO exchange has one frame outstanding, so anything close to
    // this is already pathological.
    static constexpr size_t kMaxQueued = 4096;

    struct Impl;

    std::string txKey_;
    std::string rxKey_;
    std::unique_ptr<Impl> impl_;

    mutable std::mutex mutex_;
    std::deque<helpers::CanFrame> received_;
    uint64_t dropped_ { 0 };
};

} // namespace canopen

#endif // CANOPEN_ZENOH_BUS_H
