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
// for one reason: this file compiles and is tested on macOS. The alignment
// arithmetic and the nesting are the parts that are fiddly and silently wrong,
// and they do not depend on being on Linux, so they get checked on a machine
// that cannot run them. What the constants below are worth off Linux is a
// smaller question, answered where they are defined.
#ifndef CAN_SOCKETCAN_NETLINK_H
#define CAN_SOCKETCAN_NETLINK_H

#include "can/bitrate.h"
#include "can/error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if defined(__linux__)
#include <linux/can/netlink.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#endif

namespace can::socketcan
{

// --- the kernel's constants -------------------------------------------------
//
// On Linux these are the kernel's own, taken from the headers included above.
// Nothing else is possible to get right: an attribute identifier that disagrees
// with the kernel's names a different attribute, and the kernel answers by
// ignoring it or refusing the message rather than by saying so.
//
// Off Linux the headers do not exist and the literals stand in, so that the
// encoder below still compiles and its padding and nesting can be tested. They
// are the values the kernel has, but nothing here depends on that -- the tests
// that run off Linux check the shape of a message, not which attributes it
// names. Treat them as scaffolding for the build, not as a second source of
// truth; the numbers that matter are the ones on the left of the #if.

#if defined(__linux__)

inline constexpr uint16_t kRtmNewLink = RTM_NEWLINK;

inline constexpr uint16_t kFlagRequest = NLM_F_REQUEST;
inline constexpr uint16_t kFlagAck = NLM_F_ACK;

inline constexpr uint16_t kIflaIfname = IFLA_IFNAME;
inline constexpr uint16_t kIflaLinkInfo = IFLA_LINKINFO;
inline constexpr uint16_t kIflaInfoKind = IFLA_INFO_KIND;
inline constexpr uint16_t kIflaInfoData = IFLA_INFO_DATA;

inline constexpr uint16_t kIflaCanBittiming = IFLA_CAN_BITTIMING;
inline constexpr uint16_t kIflaCanCtrlMode = IFLA_CAN_CTRLMODE;
inline constexpr uint16_t kIflaCanDataBittiming = IFLA_CAN_DATA_BITTIMING;
inline constexpr uint16_t kIflaCanState = IFLA_CAN_STATE;
inline constexpr uint16_t kIflaCanBerrCounter = IFLA_CAN_BERR_COUNTER;
inline constexpr uint16_t kIflaCanDataBittimingConst = IFLA_CAN_DATA_BITTIMING_CONST;

inline constexpr uint16_t kRtmGetLink = RTM_GETLINK;
inline constexpr uint16_t kIflaInfoXstats = IFLA_INFO_XSTATS;

inline constexpr uint32_t kIffUp = IFF_UP;

// CAN control mode bits.
inline constexpr uint32_t kCanCtrlModeLoopback = CAN_CTRLMODE_LOOPBACK;
inline constexpr uint32_t kCanCtrlModeListenOnly = CAN_CTRLMODE_LISTENONLY;
inline constexpr uint32_t kCanCtrlModeFd = CAN_CTRLMODE_FD;

// struct can_bittiming as the kernel lays it out: eight 32-bit fields. Only
// bitrate, sample_point, sjw, prop_seg, phase_seg1, phase_seg2 and brp are
// meaningful to set; the kernel fills in the rest.
inline constexpr size_t kCanBittimingSize = sizeof(struct can_bittiming);

#else

inline constexpr uint16_t kRtmNewLink = 16;

inline constexpr uint16_t kFlagRequest = 0x0001;
inline constexpr uint16_t kFlagAck = 0x0004;

inline constexpr uint16_t kIflaIfname = 3;
inline constexpr uint16_t kIflaLinkInfo = 18;
inline constexpr uint16_t kIflaInfoKind = 1;
inline constexpr uint16_t kIflaInfoData = 2;

inline constexpr uint16_t kIflaCanBittiming = 1;
inline constexpr uint16_t kIflaCanCtrlMode = 5;
inline constexpr uint16_t kIflaCanDataBittiming = 9;
inline constexpr uint16_t kIflaCanState = 4;
inline constexpr uint16_t kIflaCanBerrCounter = 8;
inline constexpr uint16_t kIflaCanDataBittimingConst = 10;

inline constexpr uint16_t kRtmGetLink = 18;
inline constexpr uint16_t kIflaInfoXstats = 3;

inline constexpr uint32_t kIffUp = 0x1;

inline constexpr uint32_t kCanCtrlModeLoopback = 0x01;
inline constexpr uint32_t kCanCtrlModeListenOnly = 0x02;
inline constexpr uint32_t kCanCtrlModeFd = 0x20;

inline constexpr size_t kCanBittimingSize = 32;

#endif

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

// --- asking what the interface actually is ----------------------------------
//
// Everything above writes. This reads, and it exists because a backend that
// only writes reports what it asked for rather than what it got: without
// CAP_NET_ADMIN every request here is refused, and a channel that believed its
// own requests published "up at 500 kbit/s" for an interface that was down and
// unconfigured. A read needs no privileges at all, so the truth is always
// available -- it just has to be asked for.

// The controller's error state, as the kernel's `enum can_state` numbers it.
// Named separately from can::BusState because this is a wire encoding: the
// kernel picked these values and they cannot be reordered to suit us.
enum class CanState : uint32_t
{
    ErrorActive = 0,
    ErrorWarning = 1,
    ErrorPassive = 2,
    BusOff = 3,
    Stopped = 4,
    Sleeping = 5,
};

// What the kernel says, as opposed to what was asked for. Every field is
// optional in the sense that the kernel omits attributes it has nothing to say
// about -- an interface that has never been given a bit rate carries no
// IFLA_CAN_BITTIMING at all, which is different from carrying a zero.
struct LinkState
{
    // From ifinfomsg's flags, not from a CAN attribute: it is the same IFF_UP
    // that `ip link` shows, and it is the one that decides whether a frame can
    // move.
    bool up { false };

    std::optional<uint32_t> nominalBps;
    std::optional<uint16_t> nominalSamplePointPermille;
    std::optional<uint32_t> dataBps;
    std::optional<uint16_t> dataSamplePointPermille;

    // Absent when the kernel sent no IFLA_CAN_CTRLMODE.
    std::optional<bool> listenOnly;
    std::optional<bool> fdEnabled;

    // True when the controller advertises a data-phase timing table, which is
    // the thing that distinguishes an FD controller from a classic one. It is
    // a property of the hardware and does not change with configuration, so it
    // answers "can this do FD" where fdEnabled answers "is it doing FD".
    bool fdCapable { false };

    std::optional<CanState> state;

    std::optional<uint16_t> rxErrorCounter;
    std::optional<uint16_t> txErrorCounter;

    // From the IFLA_INFO_XSTATS block: cumulative since the interface
    // appeared, so a bus-off that happened and recovered still shows here
    // where the instantaneous `state` no longer does.
    std::optional<uint32_t> busOffCount;
    std::optional<uint32_t> restartCount;
};

// An RTM_GETLINK asking about one interface by name. Unprivileged.
Result<std::vector<uint8_t>> encode_link_query(const std::string& interface, uint32_t sequence);

// Parses the RTM_NEWLINK the kernel answers with. Fails when the reply is
// malformed or is not about a CAN interface; an attribute that is simply
// absent is not an error, it is a `nullopt` above.
//
// Extra attributes are ignored rather than rejected. The kernel gains them
// between versions, and a reader that refused an unfamiliar one would stop
// working on the next kernel for no reason.
Result<LinkState> decode_link_state(std::span<const uint8_t> bytes);

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
