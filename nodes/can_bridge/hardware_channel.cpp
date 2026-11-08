// SPDX-License-Identifier: GPL-3.0-or-later

#include "hardware_channel.h"

namespace can_bridge
{

can::OpenOptions openOptions(const ChannelConfig& config)
{
    can::OpenOptions open;
    open.bitrate.nominalBps = config.bitrateBps;
    open.bitrate.dataBps = config.dataBitrateBps;
    open.bitrate.nominalSamplePointPermille = config.samplePointPermille;
    open.bitrate.dataSamplePointPermille = config.dataSamplePointPermille;
    open.listenOnly = config.listenOnly;
    open.rxQueueDepth = config.rxQueueDepth;
    open.start = true;
    return open;
}

}  // namespace can_bridge
