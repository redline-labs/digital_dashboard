// The simulated device's CAN port as a can::Channel, so what carries a real adapter's bus -- can_zenoh's bridge, a
// .trc tap, a tool written against can::Channel -- carries the simulator's without knowing the difference.
//
// Two threads meet here. The simulation's own thread owns the device: it hands over what the device transmitted
// (deliver) and collects what the outside world sent (take_outbound) between slices of simulated time, because a
// frame can only reach the device at a known instant of ITS clock. Every other thread sees an ordinary channel:
// send() queues for the device, receive() blocks for the device's frames.
//
// The timestamps on received frames are the simulation's own clock, in microseconds since power-on, as an adapter's
// are its own: comparable with each other, not with the wall.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "can/channel.h"

namespace sim::sys {

class SimCanChannel final : public can::Channel {
   public:
    // `device` names the simulated device for the status topic and the trace header ("sim:pdm32").
    explicit SimCanChannel(const std::string& device, size_t queue_depth = 8192);

    // ---- the simulation's thread
    // The device put this frame on its bus at `sim_us`.
    void deliver(const helpers::CanFrame& frame, uint64_t sim_us);
    // Frames sent to the device since the last call, oldest first.
    std::vector<helpers::CanFrame> take_outbound();
    // What the device's controller is programmed to, for bitrate().
    void set_device_bitrate(uint32_t bps);

    // ---- can::Channel
    const can::ChannelId& id() const override {
        return id_;
    }
    const std::string& description() const override {
        return description_;
    }
    can::Result<void> set_bitrate(const can::Bitrate& bitrate) override;
    can::Bitrate bitrate() const override;
    bool supports_fd() const override {
        return false;
    }
    can::Result<void> set_listen_only(bool listen_only) override;
    bool listen_only() const override;
    can::Result<void> start() override;
    can::Result<void> stop() override;
    bool running() const override;
    can::Result<void> send(const helpers::CanFrame& frame) override;
    can::Result<size_t> receive(std::span<helpers::CanFrame> out, can::Duration timeout) override;
    can::Statistics statistics() const override;

   private:
    can::ChannelId id_;
    std::string description_;
    size_t queue_depth_;

    mutable std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<helpers::CanFrame> inbound_;   // from the device, for receive()
    std::deque<helpers::CanFrame> outbound_;  // for the device, from send()
    uint32_t device_bps_ = 0;
    bool running_ = true;
    bool listen_only_ = false;
    can::Statistics statistics_{};
};

}  // namespace sim::sys
