// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bridged channel that is an adapter: the can_zenoh bridge, plus what it
// takes to open the hardware again when it goes away.
#ifndef CAN_BRIDGE_HARDWARE_CHANNEL_H_
#define CAN_BRIDGE_HARDWARE_CHANNEL_H_

#include "node_config.h"

#include "can/backend.h"
#include "can_zenoh/bridged_channel.h"

#include <memory>
#include <string>
#include <utility>

namespace can_bridge
{

// How a configured channel is opened, at startup and on every reopen.
can::OpenOptions openOptions(const ChannelConfig& config);

class HardwareChannel : public can_zenoh::BridgedChannel
{
  public:
    HardwareChannel(ChannelConfig config, std::shared_ptr<can::Channel> channel)
        : BridgedChannel(config, std::move(channel))
        , hardware_(std::move(config))
    {
    }

    // The whole configured channel, bit rates included: what a reopen needs.
    const ChannelConfig& hardware() const { return hardware_; }

  private:
    ChannelConfig hardware_;
};

// A channel that could not be opened. Kept rather than dropped, so the status
// topic reports what is wrong with it instead of it simply being absent -- "the
// adapter is held by the kernel driver" is a far more useful thing to publish
// than nothing at all -- and so it can be opened when the adapter turns up.
struct FailedChannel
{
    ChannelConfig config;
    std::string error;
    can_zenoh::ReopenBackoff reopen;
};

}  // namespace can_bridge

#endif  // CAN_BRIDGE_HARDWARE_CHANNEL_H_
