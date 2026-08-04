// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_socketcan/netlink.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <cstring>

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
    // IFLA_IFNAME is 3.
    put_attribute(body, 3, nameBytes);

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

} // namespace can::socketcan
