// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bus with no wire under it: a virtual clock, a queue of frames in flight,
// and somewhere to hang a simulated device.
//
// Time only moves when poll() is called, so a one-second SDO timeout costs a
// handful of loop iterations rather than a second, and a test that waits for a
// boot-up heartbeat either sees it or does not -- there is no race to lose.
//
// It also carries a bit rate. That looks like over-modelling for something with
// no wire, until you remember that the failure this library exists to avoid is
// changing a device's bit rate and then being unable to find it: a device
// answering only at its own rate is the thing that makes a mis-ordered
// reconfiguration fail in a test instead of on a bench.
#ifndef CANOPEN_VIRTUAL_BUS_H
#define CANOPEN_VIRTUAL_BUS_H

#include "canopen/bus.h"

#include <cstdint>
#include <vector>

namespace canopen
{

class VirtualBus : public Bus
{
public:
    // Anything that wants to answer frames: a simulated device, a script.
    using Device = std::function<void(const helpers::CanFrame&)>;

    explicit VirtualBus(uint32_t bitrateKbps = 250);

    void send(const helpers::CanFrame& frame) override;
    void poll(Duration budget) override;
    Clock::time_point now() const override;

    // Devices see every frame put on the bus, in attachment order.
    void attach(Device device);

    // A device answering. `after` is how long the answer takes to arrive,
    // which is what makes a timeout test possible: a device that takes longer
    // than the client's timeout is a device that timed out.
    //
    // `senderBitrateKbps` is the rate the sender was running at, and a frame
    // is dropped on delivery if the bus has since moved to a different one.
    // That is not pedantry: after an LSS reconfiguration the device's boot-up
    // frame goes out at the *new* rate, and a client that has not followed it
    // there cannot hear it. Modelling that is what makes a tool which forgets
    // to switch its interface fail here rather than on a bench. Zero means
    // "whatever the bus is running", for a caller with no device behind it.
    void inject(const helpers::CanFrame& frame, Duration after = Duration { 0 },
                uint32_t senderBitrateKbps = 0);

    // What has been sent, for tests that assert on the wire rather than on the
    // outcome -- the two-step COB-ID write, for instance, is only visible here.
    const std::vector<helpers::CanFrame>& sent() const { return sent_; }
    void clear_sent() { sent_.clear(); }

    // The rate the bus is running at. Changing it models a client that has
    // reconfigured its own interface; a device configured for a different rate
    // will not hear anything.
    uint32_t bitrate_kbps() const { return bitrateKbps_; }
    void set_bitrate_kbps(uint32_t kbps) { bitrateKbps_ = kbps; }

private:
    struct Scheduled
    {
        Clock::time_point at;
        helpers::CanFrame frame;
        uint32_t senderBitrateKbps { 0 };
    };

    // Started at a non-zero point so that subtracting a timeout from it cannot
    // wrap into a time before the epoch.
    Clock::time_point now_ { Clock::time_point {} + std::chrono::hours(1) };
    uint32_t bitrateKbps_;
    std::vector<Scheduled> queue_;
    std::vector<helpers::CanFrame> sent_;
    std::vector<Device> devices_;
};

} // namespace canopen

#endif // CANOPEN_VIRTUAL_BUS_H
