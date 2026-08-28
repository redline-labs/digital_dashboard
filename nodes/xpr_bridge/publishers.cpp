// SPDX-License-Identifier: GPL-3.0-or-later

#include "publishers.h"

namespace xpr_node
{

void SharedState::setChannel(const ChannelState& channel)
{
    const std::lock_guard<std::mutex> lock(mMutex);
    mChannel = channel;
}

std::optional<ChannelState> SharedState::channel() const
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return mChannel;
}

void SharedState::setIdentity(const xpr::Radio::Identity& identity)
{
    const std::lock_guard<std::mutex> lock(mMutex);
    mIdentity = identity;
}

std::optional<xpr::Radio::Identity> SharedState::identity() const
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return mIdentity;
}

void SharedState::clear()
{
    const std::lock_guard<std::mutex> lock(mMutex);
    mChannel.reset();
    mIdentity.reset();
}

void fillChannel(::XprChannel::Builder out, const ChannelState& in)
{
    out.setZone(in.zone);
    out.setChannel(in.channel);
    out.setZoneCount(in.zoneCount);
    out.setChannelsInZone(in.channelsInZone);
    out.setFromBroadcast(in.fromBroadcast);
}

void fillIdentity(::XprIdentity::Builder out, const xpr::Radio::Identity& in)
{
    out.setModelNumber(in.modelNumber);
    out.setSerialNumber(in.serialNumber);
    out.setFirmwareVersion(in.firmwareVersion);
    out.setTanapaNumber(in.tanapaNumber);
    out.setRadioId(in.radioId);
    out.setRadioIdKnown(in.radioIdKnown);
    out.setDatecode(::kj::arrayPtr(in.datecode.data(), in.datecode.size()));
}

Publishers::Publishers(const PublishConfig& config) : mConfig(config)
{
}

void Publishers::publishChannel(const ChannelState& channel)
{
    if (!mChannel.has_value())
    {
        mChannel.emplace(mConfig.topicPrefix + "/channel");
    }

    fillChannel(mChannel->fields(), channel);
    mChannel->put();
}

void Publishers::publishDisplay(std::uint8_t changedLine,
                                const std::array<std::string, kDisplayLines>& lines)
{
    if (!mConfig.publishDisplay)
    {
        return;
    }

    if (!mDisplay.has_value())
    {
        mDisplay.emplace(mConfig.topicPrefix + "/display");
    }

    ::XprDisplay::Builder out = mDisplay->fields();
    ::capnp::List<::capnp::Text>::Builder text = out.initLines(static_cast<unsigned>(lines.size()));
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        text.set(static_cast<unsigned>(i), lines[i]);
    }
    out.setChangedLine(changedLine);
    mDisplay->put();
}

void Publishers::publishBroadcast(const xpr::Broadcast& broadcast)
{
    if (!mConfig.publishUnknownBroadcasts)
    {
        return;
    }

    if (!mBroadcast.has_value())
    {
        mBroadcast.emplace(mConfig.topicPrefix + "/broadcast");
    }

    ::XprBroadcast::Builder out = mBroadcast->fields();
    out.setOpcode(broadcast.opcode);
    out.setName(broadcast.name);
    out.setPayload(::kj::arrayPtr(broadcast.payload.data(), broadcast.payload.size()));
    mBroadcast->put();
}

} // namespace xpr_node
