// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_socketcan/netlink.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

namespace can::socketcan
{
namespace
{

void put_u16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value)
{
    put_u16(out, static_cast<uint16_t>(value & 0xFFFF));
    put_u16(out, static_cast<uint16_t>(value >> 16));
}

uint16_t get_u16(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t get_u32(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void pad_to_align(std::vector<uint8_t>& out)
{
    while (out.size() % 4 != 0)
    {
        out.push_back(0);
    }
}

// One attribute: a 4-byte header (length including the header, then type)
// followed by the payload, padded to a 4-byte boundary. The recorded length
// does NOT include the padding, which is the detail that makes a hand-built
// message either parse or run off the end.
void put_attribute(std::vector<uint8_t>& out, uint16_t type, std::span<const uint8_t> payload)
{
    const uint16_t length = static_cast<uint16_t>(4 + payload.size());
    put_u16(out, length);
    put_u16(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    pad_to_align(out);
}

} // namespace

bool is_valid_interface_name(const std::string& name)
{
    if (name.empty() || name.size() > kMaxInterfaceNameLength)
    {
        return false;
    }
    // Anything a kernel interface name can be. Deliberately strict: this
    // string ends up in a fixed-size field, and a name with a slash or a NUL
    // in it is either a mistake or an attempt to make one.
    return std::all_of(name.begin(), name.end(),
                       [](unsigned char c)
                       { return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.'; });
}

std::vector<uint8_t> encode_bittiming(const BitTiming& timing)
{
    // struct can_bittiming, in order:
    //   bitrate, sample_point, tq, prop_seg, phase_seg1, phase_seg2, sjw, brp
    //
    // sample_point is in per-mille here as it is in the kernel. tq is derived
    // by the kernel from brp and the clock, so it is left zero.
    std::vector<uint8_t> out;
    put_u32(out, timing.bitrateBps);
    put_u32(out, timing.samplePointPermille);
    put_u32(out, 0); // tq, filled in by the kernel
    // The bus cannot tell a propagation segment from a phase segment -- only
    // their sum matters -- so the whole of tseg1 goes in phase_seg1 rather
    // than being split on an arbitrary rule.
    put_u32(out, 0); // prop_seg
    put_u32(out, timing.tseg1);
    put_u32(out, timing.tseg2);
    put_u32(out, timing.sjw);
    put_u32(out, timing.brp);
    return out;
}

Result<std::vector<uint8_t>> encode_link_request(const LinkRequest& request, uint32_t sequence)
{
    if (!is_valid_interface_name(request.interface))
    {
        return invalid_argument(fmt::format(
            "'{}' is not a valid network interface name (1..{} characters, letters, digits, "
            "'-', '_' and '.')",
            request.interface, kMaxInterfaceNameLength));
    }

    // --- the nested link-info block ----------------------------------------
    std::vector<uint8_t> infoData;
    if (request.nominal.has_value())
    {
        put_attribute(infoData, kIflaCanBittiming, encode_bittiming(*request.nominal));
    }
    if (request.data.has_value())
    {
        put_attribute(infoData, kIflaCanDataBittiming, encode_bittiming(*request.data));
    }
    if (request.listenOnly.has_value() || request.fd.has_value())
    {
        // struct can_ctrlmode is a mask and a set of flags: the mask says
        // which bits this message is speaking about, so a message that turns
        // FD on does not implicitly turn loopback off.
        uint32_t mask = 0;
        uint32_t flags = 0;
        if (request.listenOnly.has_value())
        {
            mask |= kCanCtrlModeListenOnly;
            if (*request.listenOnly)
            {
                flags |= kCanCtrlModeListenOnly;
            }
        }
        if (request.fd.has_value())
        {
            mask |= kCanCtrlModeFd;
            if (*request.fd)
            {
                flags |= kCanCtrlModeFd;
            }
        }

        std::vector<uint8_t> ctrlmode;
        put_u32(ctrlmode, mask);
        put_u32(ctrlmode, flags);
        put_attribute(infoData, kIflaCanCtrlMode, ctrlmode);
    }

    std::vector<uint8_t> linkInfo;
    if (!infoData.empty())
    {
        const std::string kind = "can";
        std::vector<uint8_t> kindBytes(kind.begin(), kind.end());
        kindBytes.push_back('\0');
        put_attribute(linkInfo, kIflaInfoKind, kindBytes);
        put_attribute(linkInfo, kIflaInfoData, infoData);
    }

    // --- the message itself -------------------------------------------------
    std::vector<uint8_t> body;

    // struct ifinfomsg: family, pad, type, index, flags, change.
    body.push_back(0); // AF_UNSPEC
    body.push_back(0); // padding
    put_u16(body, 0);  // ifi_type, unused for a set
    put_u32(body, 0);  // ifi_index: zero, because the name below identifies it
    if (request.up.has_value())
    {
        put_u32(body, *request.up ? kIffUp : 0u);
        // `change` is the mask of flags this message is speaking about, in the
        // same way ctrlmode's mask is. Setting it to IFF_UP means "only touch
        // the up bit" -- a message that changed every flag would knock out
        // whatever else the interface had set.
        put_u32(body, kIffUp);
    }
    else
    {
        put_u32(body, 0);
        put_u32(body, 0);
    }

    std::vector<uint8_t> nameBytes(request.interface.begin(), request.interface.end());
    nameBytes.push_back('\0');
    put_attribute(body, kIflaIfname, nameBytes);

    if (!linkInfo.empty())
    {
        put_attribute(body, kIflaLinkInfo, linkInfo);
    }

    std::vector<uint8_t> message;
    // struct nlmsghdr: len, type, flags, seq, pid.
    put_u32(message, static_cast<uint32_t>(16 + body.size()));
    put_u16(message, kRtmNewLink);
    put_u16(message, kFlagRequest | kFlagAck);
    put_u32(message, sequence);
    put_u32(message, 0); // pid: the kernel fills this in
    message.insert(message.end(), body.begin(), body.end());

    return message;
}

Result<NetlinkAck> decode_ack(std::span<const uint8_t> bytes)
{
    constexpr size_t kHeaderSize = 16;
    if (bytes.size() < kHeaderSize)
    {
        return protocol_error(fmt::format("a netlink reply is at least {} bytes, got {}",
                                          kHeaderSize, bytes.size()));
    }

    const uint32_t length = get_u32(bytes, 0);
    const uint16_t type = get_u16(bytes, 4);
    const uint32_t sequence = get_u32(bytes, 8);

    if (length > bytes.size())
    {
        return protocol_error(fmt::format(
            "a netlink reply claims {} bytes but only {} arrived", length, bytes.size()));
    }

    constexpr uint16_t kNlmsgError = 2;
    constexpr uint16_t kNlmsgDone = 3;

    if (type == kNlmsgDone)
    {
        return NetlinkAck { sequence, 0 };
    }
    if (type != kNlmsgError)
    {
        return protocol_error(
            fmt::format("expected a netlink acknowledgement, got message type {}", type));
    }

    if (bytes.size() < kHeaderSize + 4)
    {
        return protocol_error("a netlink error reply carries no error code");
    }

    // The kernel reports a negative errno; zero means the request succeeded.
    const int32_t reported = static_cast<int32_t>(get_u32(bytes, kHeaderSize));
    return NetlinkAck { sequence, reported < 0 ? -reported : reported };
}

Result<std::vector<uint8_t>> encode_link_query(const std::string& interface, uint32_t sequence)
{
    if (!is_valid_interface_name(interface))
    {
        return invalid_argument(fmt::format(
            "'{}' is not a valid network interface name (1..{} characters, letters, digits, "
            "'-', '_' and '.')",
            interface, kMaxInterfaceNameLength));
    }

    std::vector<uint8_t> body;

    // struct ifinfomsg. Index zero with an IFLA_IFNAME attribute asks about
    // one named interface; a dump of every link would work too and is what
    // `ip link` does, but it means filtering the answer and paying for every
    // interface on the machine on a path that runs once a second.
    body.push_back(0); // AF_UNSPEC
    body.push_back(0); // padding
    put_u16(body, 0);  // ifi_type
    put_u32(body, 0);  // ifi_index
    put_u32(body, 0);  // ifi_flags
    put_u32(body, 0);  // ifi_change

    std::vector<uint8_t> nameBytes(interface.begin(), interface.end());
    nameBytes.push_back('\0');
    put_attribute(body, kIflaIfname, nameBytes);

    std::vector<uint8_t> message;
    put_u32(message, static_cast<uint32_t>(16 + body.size()));
    put_u16(message, kRtmGetLink);
    // No NLM_F_ACK: the answer to a GETLINK is the link itself, and asking for
    // an acknowledgement as well would put an NLMSG_ERROR in front of it.
    put_u16(message, kFlagRequest);
    put_u32(message, sequence);
    put_u32(message, 0);
    message.insert(message.end(), body.begin(), body.end());

    return message;
}

namespace
{

// Walks a run of netlink attributes, calling `visit(type, payload)` for each.
// Stops at the first attribute whose recorded length does not fit, which is
// how a truncated reply ends without reading past the buffer.
template <typename Visitor>
void for_each_attribute(std::span<const uint8_t> bytes, Visitor visit)
{
    size_t offset = 0;
    while (offset + 4 <= bytes.size())
    {
        const uint16_t length = get_u16(bytes, offset);
        const uint16_t type = get_u16(bytes, offset + 2);
        if (length < 4 || offset + length > bytes.size())
        {
            return;
        }
        visit(type, bytes.subspan(offset + 4, length - 4u));
        offset += align4(length);
    }
}

// struct can_bittiming's first two fields. The rest -- tq and the segment
// split -- are the kernel's derivation of what we asked for and say nothing a
// caller of this library needs.
void read_bittiming(std::span<const uint8_t> payload, std::optional<uint32_t>& bps,
                    std::optional<uint16_t>& samplePoint)
{
    if (payload.size() < 8)
    {
        return;
    }
    const uint32_t bitrate = get_u32(payload, 0);
    // A configured interface reports a non-zero rate. Zero means the attribute
    // is there but the controller has never been timed, which is "unset", not
    // "zero bits per second".
    if (bitrate == 0)
    {
        return;
    }
    bps = bitrate;
    samplePoint = static_cast<uint16_t>(get_u32(payload, 4));
}

void read_can_attributes(std::span<const uint8_t> payload, LinkState& state)
{
    for_each_attribute(
        payload,
        [&state](uint16_t type, std::span<const uint8_t> value)
        {
            if (type == kIflaCanBittiming)
            {
                read_bittiming(value, state.nominalBps, state.nominalSamplePointPermille);
            }
            else if (type == kIflaCanDataBittiming)
            {
                read_bittiming(value, state.dataBps, state.dataSamplePointPermille);
            }
            else if (type == kIflaCanDataBittimingConst)
            {
                // Presence is the whole signal: a controller that cannot do FD
                // has no data-phase timing table to advertise.
                state.fdCapable = true;
            }
            else if (type == kIflaCanCtrlMode && value.size() >= 8)
            {
                // struct can_ctrlmode { mask, flags }. In a reply the mask is
                // what the controller supports and the flags are what is on,
                // so the flags are the ones to read.
                const uint32_t flags = get_u32(value, 4);
                state.listenOnly = (flags & kCanCtrlModeListenOnly) != 0;
                state.fdEnabled = (flags & kCanCtrlModeFd) != 0;
            }
            else if (type == kIflaCanState && value.size() >= 4)
            {
                const uint32_t raw = get_u32(value, 0);
                if (raw <= static_cast<uint32_t>(CanState::Sleeping))
                {
                    state.state = static_cast<CanState>(raw);
                }
            }
            else if (type == kIflaCanBerrCounter && value.size() >= 4)
            {
                // struct can_berr_counter { __u16 txerr; __u16 rxerr; } -- tx
                // first, which is the opposite order from how they are usually
                // written and quoted.
                state.txErrorCounter = get_u16(value, 0);
                state.rxErrorCounter = get_u16(value, 2);
            }
        });
}

// struct can_device_stats: six counters, of which bus_off and restarts are the
// two that say something the instantaneous state does not.
void read_xstats(std::span<const uint8_t> payload, LinkState& state)
{
    if (payload.size() < 24)
    {
        return;
    }
    state.busOffCount = get_u32(payload, 12);
    state.restartCount = get_u32(payload, 20);
}

} // namespace

Result<LinkState> decode_link_state(std::span<const uint8_t> bytes)
{
    // nlmsghdr (16) + ifinfomsg (16).
    constexpr size_t kHeaderSize = 16;
    constexpr size_t kIfInfoSize = 16;

    if (bytes.size() < kHeaderSize + kIfInfoSize)
    {
        return protocol_error(fmt::format("a link reply is at least {} bytes, got {}",
                                          kHeaderSize + kIfInfoSize, bytes.size()));
    }

    const uint32_t length = get_u32(bytes, 0);
    const uint16_t type = get_u16(bytes, 4);

    constexpr uint16_t kNlmsgError = 2;
    if (type == kNlmsgError)
    {
        auto ack = decode_ack(bytes);
        if (!ack.has_value())
        {
            return std::unexpected(ack.error());
        }
        return protocol_error(
            fmt::format("the kernel refused the link query: error {}", ack->error));
    }
    if (type != kRtmNewLink)
    {
        return protocol_error(
            fmt::format("expected a link reply, got message type {}", type));
    }
    if (length > bytes.size())
    {
        return protocol_error(fmt::format(
            "a link reply claims {} bytes but only {} arrived", length, bytes.size()));
    }

    LinkState state;
    // ifinfomsg's flags sit 8 bytes into it: family, pad, type (4), index (4).
    state.up = (get_u32(bytes, kHeaderSize + 8) & kIffUp) != 0;

    bool isCan = false;
    // A reply that declares a length shorter than its own headers has no
    // attribute run at all; clamping rather than subtracting keeps that case
    // from wrapping into an enormous span.
    const size_t declared = std::max<size_t>(length, kHeaderSize + kIfInfoSize);
    const size_t bodyEnd = std::min<size_t>(declared, bytes.size());
    for_each_attribute(
        bytes.subspan(kHeaderSize + kIfInfoSize, bodyEnd - kHeaderSize - kIfInfoSize),
        [&state, &isCan](uint16_t attribute, std::span<const uint8_t> value)
        {
            if (attribute != kIflaLinkInfo)
            {
                return;
            }
            for_each_attribute(
                value,
                [&state, &isCan](uint16_t inner, std::span<const uint8_t> nested)
                {
                    if (inner == kIflaInfoKind)
                    {
                        // NUL-terminated in the message; compare without it.
                        const size_t n = nested.empty() || nested.back() != 0 ? nested.size()
                                                                             : nested.size() - 1;
                        isCan = std::string_view(reinterpret_cast<const char*>(nested.data()), n)
                            == "can";
                    }
                    else if (inner == kIflaInfoData)
                    {
                        read_can_attributes(nested, state);
                    }
                    else if (inner == kIflaInfoXstats)
                    {
                        read_xstats(nested, state);
                    }
                });
        });

    if (!isCan)
    {
        return protocol_error("the interface in this link reply is not a CAN interface");
    }

    return state;
}

} // namespace can::socketcan
