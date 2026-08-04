// SPDX-License-Identifier: GPL-3.0-or-later
//
// The netlink messages that configure a SocketCAN interface, built as bytes.
//
// Changing a CAN interface's bit rate is not something a socket option can do.
// It is a link-layer property, set the same way `ip link set can0 type can
// bitrate 500000` sets it: an RTM_NEWLINK message carrying a nested
// IFLA_LINKINFO/IFLA_INFO_DATA block. Bringing the interface up and down is the
// same message with different flags. Shelling out to `ip` would work and would
// be shorter, but it puts a parser for another program's output in the path of
// every configuration change and needs `ip` to exist on the target.
//
// The message construction is here, separately from the socket that sends it,
// for one reason: this file compiles and is tested on macOS. Netlink is a Linux
// interface and the structures come from Linux headers, so the constants and
// layouts are declared here rather than included -- which means the alignment
// arithmetic and the nesting, the parts that are fiddly and silently wrong, are
// checked on a machine that cannot run them.
#ifndef CAN_SOCKETCAN_NETLINK_H
#define CAN_SOCKETCAN_NETLINK_H

#include "can/bitrate.h"
#include "can/error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace can::socketcan
{

// --- constants, declared rather than included ------------------------------
//
// These come from <linux/netlink.h>, <linux/rtnetlink.h>, <linux/if.h> and
// <linux/can/netlink.h>. Repeating them means this file builds anywhere; the
// Linux build asserts they still agree with the kernel's own headers.

inline constexpr uint16_t kRtmNewLink = 16;

inline constexpr uint16_t kFlagRequest = 0x0001;
inline constexpr uint16_t kFlagAck = 0x0004;

inline constexpr uint16_t kIflaLinkInfo = 18;
inline constexpr uint16_t kIflaInfoKind = 1;
inline constexpr uint16_t kIflaInfoData = 2;

inline constexpr uint16_t kIflaCanBittiming = 1;
inline constexpr uint16_t kIflaCanCtrlMode = 3;
inline constexpr uint16_t kIflaCanDataBittiming = 8;

inline constexpr uint32_t kIffUp = 0x1;

// CAN control mode bits.
inline constexpr uint32_t kCanCtrlModeLoopback = 0x01;
inline constexpr uint32_t kCanCtrlModeListenOnly = 0x02;
inline constexpr uint32_t kCanCtrlModeFd = 0x20;

// struct can_bittiming as the kernel lays it out: eight 32-bit fields. Only
// bitrate, sample_point, sjw, prop_seg, phase_seg1, phase_seg2 and brp are
// meaningful to set; the kernel fills in the rest.
inline constexpr size_t kCanBittimingSize = 32;

// --- message construction --------------------------------------------------

// The bit timing the kernel wants. Note that it splits tseg1 into a propagation
// segment and a phase segment, where the controller and this library treat it
// as one number -- the split is arbitrary as far as the bus is concerned, so
// everything goes in phase_seg1 and prop_seg stays zero.
std::vector<uint8_t> encode_bittiming(const BitTiming& timing);

struct LinkRequest
{
    std::string interface;
    // Absent leaves the bit rate alone, which is what bringing an interface
    // up or down without reconfiguring it needs.
    std::optional<BitTiming> nominal;
    std::optional<BitTiming> data;
    std::optional<bool> up;
    std::optional<bool> listenOnly;
    std::optional<bool> fd;
};

// Builds a complete RTM_NEWLINK message: header, ifinfomsg, IFLA_IFNAME, and
// the nested IFLA_LINKINFO block when there is timing to set.
//
// `sequence` is echoed in the kernel's acknowledgement, so a caller can match
// the answer to the request.
Result<std::vector<uint8_t>> encode_link_request(const LinkRequest& request, uint32_t sequence);

// --- reply parsing ---------------------------------------------------------

// The kernel answers a request-with-ack with an NLMSG_ERROR carrying a
// negative errno, or zero for success.
struct NetlinkAck
{
    uint32_t sequence { 0 };
    // 0 on success, otherwise a positive errno.
    int error { 0 };
};

Result<NetlinkAck> decode_ack(std::span<const uint8_t> bytes);

// --- helpers ---------------------------------------------------------------

// Netlink pads everything to four bytes. Exposed because the padding rule is
// what makes a hand-built message either parse or be silently truncated.
constexpr size_t align4(size_t length)
{
    return (length + 3u) & ~size_t { 3 };
}

// An interface name has to fit the kernel's fixed-size field.
inline constexpr size_t kMaxInterfaceNameLength = 15;
bool is_valid_interface_name(const std::string& name);

} // namespace can::socketcan

#endif // CAN_SOCKETCAN_NETLINK_H
