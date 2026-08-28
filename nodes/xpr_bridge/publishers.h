// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node's topics, and the little state the node keeps about the radio.
//
// PUBLISHERS ARE CREATED ON FIRST USE, not up front, so the liveliness
// advertisements name what the radio is actually saying. A display topic that
// exists but has never published looks identical, in every picker in this
// tree, to one whose radio has gone quiet.
//
// EVERYTHING HERE RUNS ON THE NODE'S OWN THREAD. ZenohPublisher is not
// thread-safe, and the services deliberately do not publish: a service that
// changes the channel gets its message onto the bus the same way a knob turn
// does, through the radio's own 0xB40D broadcast, which is also the only way
// the two paths cannot disagree.

#ifndef XPR_NODE_PUBLISHERS_H
#define XPR_NODE_PUBLISHERS_H

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "node_config.h"
#include "xpr_radio.capnp.h"
#include "pub_sub/zenoh_publisher.h"
#include "xpr/radio.h"

namespace xpr_node
{

// The radio has four display lines. Index 0 is line 1.
inline constexpr std::size_t kDisplayLines = 4;

struct ChannelState
{
    std::uint16_t zone { 0 };
    std::uint16_t channel { 0 };

    // Carried forward from the last count query: the broadcasts do not include
    // them, and a channel message without them cannot say "3 of 5".
    std::uint16_t zoneCount { 0 };
    std::uint16_t channelsInZone { 0 };

    bool fromBroadcast { false };
};

// What the node knows about the radio. Written by the node's loop, read by the
// service threads, so every access takes the lock.
class SharedState
{
  public:
    void setChannel(const ChannelState& channel);
    std::optional<ChannelState> channel() const;

    void setIdentity(const xpr::Radio::Identity& identity);
    std::optional<xpr::Radio::Identity> identity() const;

    // On disconnect. A stale channel served as current is worse than none:
    // the radio may have been turned to something else while it was away.
    void clear();

  private:
    mutable std::mutex mMutex;
    std::optional<ChannelState> mChannel;
    std::optional<xpr::Radio::Identity> mIdentity;
};

// Shared between the topics and the service replies, so a channel reported by
// a service call and the same channel on the topic read identically.
void fillChannel(::XprChannel::Builder out, const ChannelState& in);
void fillIdentity(::XprIdentity::Builder out, const xpr::Radio::Identity& in);

class Publishers
{
  public:
    explicit Publishers(const PublishConfig& config);

    Publishers(const Publishers&) = delete;
    Publishers& operator=(const Publishers&) = delete;

    void publishChannel(const ChannelState& channel);
    void publishDisplay(std::uint8_t changedLine, const std::array<std::string, kDisplayLines>& lines);
    void publishBroadcast(const xpr::Broadcast& broadcast);

  private:
    PublishConfig mConfig;

    std::optional<pub_sub::ZenohPublisher<::XprChannel>> mChannel;
    std::optional<pub_sub::ZenohPublisher<::XprDisplay>> mDisplay;
    std::optional<pub_sub::ZenohPublisher<::XprBroadcast>> mBroadcast;
};

} // namespace xpr_node

#endif // XPR_NODE_PUBLISHERS_H
