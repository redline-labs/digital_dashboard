// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SocketCAN backend's byte-level halves, tested on a machine that has no
// SocketCAN.
//
// Two things are worth pulling out of the Linux-only code and checking here.
//
// The frame conversion, because the three flag bits above the identifier decide
// whether a frame means what it says. Masking with the wrong constant turns a
// 29-bit identifier into a different 11-bit one, and missing the error bit
// turns a bus-off report into traffic from a device that is not there. Both are
// silent.
//
// The netlink message construction, because it is nested, length-prefixed and
// padded, and a message the kernel cannot parse is a message the kernel ignores
// without complaint. Building it wrong looks exactly like a bit rate that did
// not take effect.

#include "golden/link_reply.h"

#include "can_socketcan/netlink.h"
#include "can_socketcan/socketcan_backend.h"
#include "can_socketcan/socketcan_frame.h"

#include "can/bitrate.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

uint32_t get_u32(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

uint16_t get_u16(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

// ============================================================================
// Frames
// ============================================================================

void test_classic_frame_round_trip()
{
    helpers::CanFrame frame {};
    frame.id = 0x123;
    frame.len = 4;
    frame.data[0] = 0xDE;
    frame.data[1] = 0xAD;
    frame.data[2] = 0xBE;
    frame.data[3] = 0xEF;

    std::array<uint8_t, can::socketcan::kFdFrameSize> buffer {};
    auto size = can::socketcan::encode_frame(frame, buffer);
    check(size.has_value() && *size == can::socketcan::kClassicFrameSize,
          "a classic frame encodes into 16 bytes");
    if (!size.has_value())
    {
        return;
    }

    check(get_u32(buffer, 0) == 0x123, "with the identifier and no flags");
    check(buffer[4] == 4, "the length in byte 4");
    check(buffer[8] == 0xDE && buffer[11] == 0xEF, "and the payload at offset 8");

    auto decoded = can::socketcan::decode_frame(std::span(buffer.data(), *size));
    check(decoded.has_value(), "and decodes back");
    if (decoded.has_value())
    {
        check(decoded->id == 0x123 && decoded->len == 4, "to the same identifier and length");
        check(!decoded->isExtended && !decoded->isFD && !decoded->isRTR && !decoded->isError,
              "with no flags set");
        check(decoded->data[0] == 0xDE && decoded->data[3] == 0xEF, "and the same payload");
    }
}

void test_extended_identifier()
{
    // The bit that decides whether 0x123 means one message or another.
    helpers::CanFrame frame {};
    frame.id = 0x1ABCDEF;
    frame.isExtended = true;
    frame.len = 1;
    frame.data[0] = 0x5A;

    std::array<uint8_t, can::socketcan::kFdFrameSize> buffer {};
    auto size = can::socketcan::encode_frame(frame, buffer);
    check(size.has_value(), "an extended frame encodes");
    if (!size.has_value())
    {
        return;
    }

    const uint32_t raw = get_u32(buffer, 0);
    check((raw & can::socketcan::kEffFlag) != 0, "with the EFF flag set");
    check((raw & can::socketcan::kEffMask) == 0x1ABCDEF, "and all 29 bits of the identifier");

    auto decoded = can::socketcan::decode_frame(std::span(buffer.data(), *size));
    check(decoded.has_value() && decoded->isExtended, "and decodes as extended");
    check(decoded.has_value() && decoded->id == 0x1ABCDEF, "with the identifier intact");

    // The same numeric identifier as a standard frame must not come back
    // extended, and must not carry the EFF bit.
    helpers::CanFrame standard {};
    standard.id = 0x7FF;
    std::array<uint8_t, can::socketcan::kFdFrameSize> standardBuffer {};
    auto standardSize = can::socketcan::encode_frame(standard, standardBuffer);
    check(standardSize.has_value() && (get_u32(standardBuffer, 0) & can::socketcan::kEffFlag) == 0,
          "an 11-bit frame does not set the EFF flag");
}

void test_error_frames()
{
    // The kernel reports bus conditions as frames with the ERR bit set. A
    // decoder that missed it would treat a bus-off report as a message.
    std::array<uint8_t, can::socketcan::kClassicFrameSize> raw {};
    const uint32_t id = can::socketcan::kErrFlag | 0x40; // some error class
    raw[0] = static_cast<uint8_t>(id & 0xFF);
    raw[1] = static_cast<uint8_t>((id >> 8) & 0xFF);
    raw[2] = static_cast<uint8_t>((id >> 16) & 0xFF);
    raw[3] = static_cast<uint8_t>((id >> 24) & 0xFF);
    raw[4] = 8;

    auto decoded = can::socketcan::decode_frame(raw);
    check(decoded.has_value(), "an error frame decodes");
    if (decoded.has_value())
    {
        check(decoded->isError, "flagged as an error report rather than a message");
        check(!decoded->isExtended, "and not mistaken for an extended-identifier frame");
    }

    // And it cannot be sent: an error frame is something the controller
    // produces, not something a node transmits.
    helpers::CanFrame error {};
    error.isError = true;
    std::array<uint8_t, can::socketcan::kFdFrameSize> buffer {};
    check(!can::socketcan::encode_frame(error, buffer).has_value(),
          "an error frame is refused for transmission");
}

void test_fd_frames()
{
    helpers::CanFrame frame {};
    frame.id = 0x100;
    frame.isFD = true;
    frame.isBRS = true;
    frame.len = 48;
    for (int i = 0; i < 48; ++i)
    {
        frame.data[i] = static_cast<uint8_t>(i);
    }

    std::array<uint8_t, can::socketcan::kFdFrameSize> buffer {};
    auto size = can::socketcan::encode_frame(frame, buffer);
    check(size.has_value() && *size == can::socketcan::kFdFrameSize,
          "an FD frame encodes into 72 bytes");
    if (!size.has_value())
    {
        return;
    }

    check(buffer[4] == 48, "the length is a real byte count, not a DLC");
    check((buffer[5] & can::socketcan::kFdFlagBrs) != 0, "with the bit-rate switch flag");

    auto decoded = can::socketcan::decode_frame(std::span(buffer.data(), *size));
    check(decoded.has_value(), "and decodes back");
    if (decoded.has_value())
    {
        check(decoded->isFD && decoded->isBRS, "as an FD frame with the switch");
        check(decoded->len == 48 && decoded->data[47] == 47, "with all 48 bytes");
    }

    // A length CAN FD cannot express is refused rather than padded, because
    // padding puts bytes on the bus the receiver cannot distinguish from data.
    helpers::CanFrame odd {};
    odd.isFD = true;
    odd.len = 10;
    check(!can::socketcan::encode_frame(odd, buffer).has_value(),
          "a 10-byte FD payload is refused");

    // Classic stops at eight.
    helpers::CanFrame tooLong {};
    tooLong.len = 9;
    check(!can::socketcan::encode_frame(tooLong, buffer).has_value(),
          "a nine-byte classic frame is refused");

    // An FD frame does not fit a classic buffer.
    std::array<uint8_t, can::socketcan::kClassicFrameSize> small {};
    check(!can::socketcan::encode_frame(frame, small).has_value(),
          "an FD frame will not be squeezed into a classic buffer");
}

void test_remote_frames()
{
    helpers::CanFrame rtr {};
    rtr.id = 0x200;
    rtr.isRTR = true;
    rtr.len = 8;

    std::array<uint8_t, can::socketcan::kFdFrameSize> buffer {};
    auto size = can::socketcan::encode_frame(rtr, buffer);
    check(size.has_value(), "a remote frame encodes");
    if (size.has_value())
    {
        check((get_u32(buffer, 0) & can::socketcan::kRtrFlag) != 0, "with the RTR flag");
        check(buffer[4] == 8, "and a length, even though it carries no payload");
        check(buffer[8] == 0, "the payload bytes staying zero");
    }
}

void test_bad_reads_are_rejected()
{
    // Anything that is not exactly one of the two structures is not a frame.
    std::array<uint8_t, 20> odd {};
    check(!can::socketcan::decode_frame(odd).has_value(),
          "a read of an unexpected size is rejected rather than decoded");
    check(!can::socketcan::decode_frame(std::span<const uint8_t> {}).has_value(),
          "so is an empty read");
}

// ============================================================================
// Netlink
// ============================================================================

void test_interface_name_validation()
{
    check(can::socketcan::is_valid_interface_name("can0"), "'can0' is a valid interface name");
    check(can::socketcan::is_valid_interface_name("vcan0"), "so is 'vcan0'");
    check(!can::socketcan::is_valid_interface_name(""), "an empty name is not");
    check(!can::socketcan::is_valid_interface_name("this_name_is_far_too_long"),
          "nor is one longer than the kernel's field");
    check(!can::socketcan::is_valid_interface_name("can0/../eth0"),
          "nor one with a slash in it -- that is either a mistake or an attempt at one");
}

void test_link_request_structure()
{
    auto timing = can::solve_bit_timing(500000, 875, can::BitTimingLimits {});
    check(timing.has_value(), "the timing solves");
    if (!timing.has_value())
    {
        return;
    }

    can::socketcan::LinkRequest request;
    request.interface = "can0";
    request.nominal = *timing;
    request.up = false;

    auto message = can::socketcan::encode_link_request(request, 42);
    check(message.has_value(), "the link request encodes");
    if (!message.has_value())
    {
        SPDLOG_ERROR("  {}", can::to_string(message.error()));
        return;
    }

    // The header's length field has to match what was actually produced, or
    // the kernel reads past the message or stops short of it.
    check(get_u32(*message, 0) == message->size(),
          fmt::format("the header length ({}) matches the message ({})", get_u32(*message, 0),
                      message->size()));
    check(get_u16(*message, 4) == can::socketcan::kRtmNewLink, "typed as RTM_NEWLINK");
    check((get_u16(*message, 6) & can::socketcan::kFlagAck) != 0,
          "asking for an acknowledgement, so a failure is reported rather than silent");
    check(get_u32(*message, 8) == 42, "carrying the sequence number back");

    // Everything is padded to four bytes, so the total must be too.
    check(message->size() % 4 == 0, "the whole message is 4-byte aligned");

    // Walk the attributes after the 16-byte header and the 16-byte ifinfomsg,
    // confirming each one's length keeps the walk inside the message. This is
    // the property that decides whether the kernel can parse it at all.
    size_t offset = 32;
    int attributes = 0;
    bool sawIfName = false;
    bool sawLinkInfo = false;
    while (offset + 4 <= message->size())
    {
        const uint16_t length = get_u16(*message, offset);
        const uint16_t type = get_u16(*message, offset + 2);
        check(length >= 4, "every attribute is at least its own header");
        check(offset + length <= message->size(),
              fmt::format("attribute {} of length {} at offset {} stays inside the message",
                          type, length, offset));
        if (length < 4 || offset + length > message->size())
        {
            break;
        }
        if (type == 3) sawIfName = true;
        if (type == can::socketcan::kIflaLinkInfo) sawLinkInfo = true;
        ++attributes;
        offset += can::socketcan::align4(length);
    }

    check(offset == message->size(), "and the attributes exactly fill the message");
    check(sawIfName, "the interface name is there");
    check(sawLinkInfo, "so is the nested link-info block carrying the bit timing");
    check(attributes == 2, "and nothing else");
}

void test_bittiming_encoding()
{
    can::BitTiming timing;
    timing.brp = 8;
    timing.tseg1 = 13;
    timing.tseg2 = 2;
    timing.sjw = 2;
    timing.bitrateBps = 500000;
    timing.samplePointPermille = 875;

    auto bytes = can::socketcan::encode_bittiming(timing);
    check(bytes.size() == can::socketcan::kCanBittimingSize,
          "struct can_bittiming is eight 32-bit fields");
    check(get_u32(bytes, 0) == 500000, "bitrate first");
    check(get_u32(bytes, 4) == 875, "then the sample point in per mille");
    check(get_u32(bytes, 8) == 0, "tq is left for the kernel to derive");
    check(get_u32(bytes, 12) == 0, "prop_seg is zero");
    check(get_u32(bytes, 16) == 13,
          "and the whole of tseg1 goes in phase_seg1 -- the bus cannot tell the two apart, so "
          "splitting them on an arbitrary rule would only invite disagreement");
    check(get_u32(bytes, 20) == 2, "tseg2");
    check(get_u32(bytes, 24) == 2, "sjw");
    check(get_u32(bytes, 28) == 8, "and the prescaler as-is, not one less");
}

void test_link_request_rejections()
{
    can::socketcan::LinkRequest bad;
    bad.interface = "";
    check(!can::socketcan::encode_link_request(bad, 1).has_value(),
          "a request with no interface name is refused before it reaches the kernel");

    bad.interface = "a_very_long_interface_name";
    check(!can::socketcan::encode_link_request(bad, 1).has_value(),
          "so is one the kernel's field cannot hold");
}

void test_ack_decoding()
{
    // Success: NLMSG_ERROR with a zero error code.
    std::vector<uint8_t> ok(20, 0);
    ok[0] = 20;
    ok[4] = 2; // NLMSG_ERROR
    ok[8] = 7; // sequence
    auto success = can::socketcan::decode_ack(ok);
    check(success.has_value() && success->error == 0, "a zero error code is success");
    check(success.has_value() && success->sequence == 7, "and the sequence comes back");

    // Failure: a negative errno.
    std::vector<uint8_t> denied(20, 0);
    denied[0] = 20;
    denied[4] = 2;
    const int32_t negativeEperm = -1;
    std::memcpy(&denied[16], &negativeEperm, sizeof(negativeEperm));
    auto failure = can::socketcan::decode_ack(denied);
    check(failure.has_value() && failure->error == 1,
          "a negative errno comes back positive, ready to be turned into a message");

    // Anything shorter than a header is not an answer.
    std::vector<uint8_t> tiny(8, 0);
    check(!can::socketcan::decode_ack(tiny).has_value(), "a truncated reply is rejected");
}

// ============================================================================
// Availability
// ============================================================================

void test_availability_is_honest()
{
#if defined(__linux__)
    check(can::socketcan::is_available(), "SocketCAN is available on Linux");
#else
    check(!can::socketcan::is_available(), "SocketCAN is not available off Linux");

    // The backend still exists, so a channel list means the same thing
    // everywhere -- and opening one says why rather than "no such backend".
    auto backend = can::socketcan::make_socketcan_backend();
    check(backend->name() == "socketcan", "and the backend is still registered under its name");
    check(backend->enumerate().empty(), "finding nothing");

    auto opened = backend->open(can::ChannelId { "socketcan", "can0", 0 }, can::OpenOptions {});
    check(!opened.has_value(), "and refusing to open");
    check(!opened.has_value() && opened.error().kind == can::Error::Kind::Unsupported,
          "as unsupported rather than not-found, because the interface is not missing -- the "
          "whole kernel subsystem is");
    check(!opened.has_value() && opened.error().message.find("Linux") != std::string::npos,
          "with a message that says so");
#endif
}

} // namespace


// ============================================================================
// Asking the kernel what an interface actually is
// ============================================================================
//
// Unlike the frame tests above, this section names the netlink types often
// enough that qualifying every one of them buries what is being asserted.
using namespace can::socketcan;

// A synthetic reply, for the states the captured one cannot show. Built with
// the same put/pad rules the encoder uses, so a padding mistake here fails
// loudly rather than quietly agreeing with a matching mistake in the decoder.
struct ReplyBuilder
{
    std::vector<uint8_t> canAttributes;
    std::vector<uint8_t> xstats;
    uint32_t flags { 0 };
    std::string kind { "can" };

    static void put_u16(std::vector<uint8_t>& out, uint16_t value)
    {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        out.push_back(static_cast<uint8_t>(value >> 8));
    }

    static void put_u32(std::vector<uint8_t>& out, uint32_t value)
    {
        put_u16(out, static_cast<uint16_t>(value & 0xFFFF));
        put_u16(out, static_cast<uint16_t>(value >> 16));
    }

    static void attribute(std::vector<uint8_t>& out, uint16_t type,
                          const std::vector<uint8_t>& payload)
    {
        put_u16(out, static_cast<uint16_t>(4 + payload.size()));
        put_u16(out, type);
        out.insert(out.end(), payload.begin(), payload.end());
        while (out.size() % 4 != 0)
        {
            out.push_back(0);
        }
    }

    static std::vector<uint8_t> bittiming(uint32_t bps, uint32_t samplePoint)
    {
        std::vector<uint8_t> out;
        put_u32(out, bps);
        put_u32(out, samplePoint);
        for (int i = 0; i < 6; ++i)
        {
            put_u32(out, 0);
        }
        return out;
    }

    ReplyBuilder& up(bool value)
    {
        flags = value ? 1u : 0u;
        return *this;
    }

    ReplyBuilder& nominal(uint32_t bps, uint32_t samplePoint)
    {
        attribute(canAttributes, kIflaCanBittiming, bittiming(bps, samplePoint));
        return *this;
    }

    ReplyBuilder& data(uint32_t bps, uint32_t samplePoint)
    {
        attribute(canAttributes, kIflaCanDataBittiming, bittiming(bps, samplePoint));
        return *this;
    }

    ReplyBuilder& fd_capable()
    {
        // The const table's contents do not matter; its presence is the signal.
        attribute(canAttributes, kIflaCanDataBittimingConst, std::vector<uint8_t>(48, 0));
        return *this;
    }

    ReplyBuilder& ctrlmode(uint32_t mask, uint32_t set)
    {
        std::vector<uint8_t> out;
        put_u32(out, mask);
        put_u32(out, set);
        attribute(canAttributes, kIflaCanCtrlMode, out);
        return *this;
    }

    ReplyBuilder& state(uint32_t value)
    {
        std::vector<uint8_t> out;
        put_u32(out, value);
        attribute(canAttributes, kIflaCanState, out);
        return *this;
    }

    ReplyBuilder& berr(uint16_t txerr, uint16_t rxerr)
    {
        std::vector<uint8_t> out;
        put_u16(out, txerr);
        put_u16(out, rxerr);
        attribute(canAttributes, kIflaCanBerrCounter, out);
        return *this;
    }

    ReplyBuilder& device_stats(uint32_t busOff, uint32_t restarts)
    {
        xstats.clear();
        put_u32(xstats, 0); // bus_error
        put_u32(xstats, 0); // error_warning
        put_u32(xstats, 0); // error_passive
        put_u32(xstats, busOff);
        put_u32(xstats, 0); // arbitration_lost
        put_u32(xstats, restarts);
        return *this;
    }

    std::vector<uint8_t> build() const
    {
        std::vector<uint8_t> linkInfo;
        std::vector<uint8_t> kindBytes(kind.begin(), kind.end());
        kindBytes.push_back('\0');
        attribute(linkInfo, kIflaInfoKind, kindBytes);
        if (!canAttributes.empty())
        {
            attribute(linkInfo, kIflaInfoData, canAttributes);
        }
        if (!xstats.empty())
        {
            attribute(linkInfo, kIflaInfoXstats, xstats);
        }

        std::vector<uint8_t> body;
        body.push_back(0); // family
        body.push_back(0); // pad
        put_u16(body, 280); // ARPHRD_CAN
        put_u32(body, 3);   // index
        put_u32(body, flags);
        put_u32(body, 0); // change

        std::vector<uint8_t> nameBytes { 'c', 'a', 'n', '0', '\0' };
        attribute(body, kIflaIfname, nameBytes);
        attribute(body, kIflaLinkInfo, linkInfo);

        std::vector<uint8_t> message;
        put_u32(message, static_cast<uint32_t>(16 + body.size()));
        put_u16(message, kRtmNewLink);
        put_u16(message, 0);
        put_u32(message, 1);
        put_u32(message, 0);
        message.insert(message.end(), body.begin(), body.end());
        return message;
    }
};

void test_link_query_structure()
{
    auto message = encode_link_query("can0", 7);
    check(message.has_value(), "a link query for a valid interface encodes");
    if (!message.has_value())
    {
        return;
    }

    check(get_u32(*message, 0) == message->size(), "the query's length field matches its size");
    check(get_u16(*message, 4) == kRtmGetLink, "the query is an RTM_GETLINK");
    // Asking for an acknowledgement as well as an answer would put an
    // NLMSG_ERROR in front of the link, which the decoder would then report as
    // a refusal.
    check((get_u16(*message, 6) & kFlagAck) == 0, "the query does not ask for an acknowledgement");
    check((get_u16(*message, 6) & kFlagRequest) != 0, "the query is a request");
    check(get_u32(*message, 8) == 7, "the query carries the sequence it was given");
    // ifinfomsg's index stays zero: the name attribute is what selects the
    // interface, and a non-zero index there would select a different one.
    check(get_u32(*message, 16 + 4) == 0, "the query leaves ifi_index zero");
    check(get_u16(*message, 32) == 4 + 5, "the name attribute is header plus 'can0\\0'");
    check(get_u16(*message, 34) == kIflaIfname, "the query names the interface by IFLA_IFNAME");
}

void test_link_query_rejections()
{
    check(!encode_link_query("", 1).has_value(), "an empty interface name is refused");
    check(!encode_link_query("this-name-is-far-too-long", 1).has_value(),
          "an over-long interface name is refused");
    check(!encode_link_query("can0/../etc", 1).has_value(),
          "an interface name with a slash is refused");
}

// The captured reply, which is the whole reason this decoder exists: an
// interface that is down and unconfigured, which the backend used to report as
// up and running at whatever rate it had asked for.
void test_real_down_interface_capture()
{
    auto state = decode_link_state(
        std::span(golden::kCan0DownReply, golden::kCan0DownReplySize));
    check(state.has_value(), "a real kernel link reply decodes");
    if (!state.has_value())
    {
        SPDLOG_ERROR("  {}", to_string(state.error()));
        return;
    }

    check(!state->up, "the captured interface reads as down");
    // The kernel sends no bittiming attribute for an interface that has never
    // been configured. Absent, not zero -- that distinction is what lets the
    // channel fall back to the requested rate instead of reporting 0 bit/s.
    check(!state->nominalBps.has_value(), "an unconfigured interface reports no bit rate");
    check(!state->dataBps.has_value(), "an unconfigured interface reports no data bit rate");
    // A PCAN-USB Pro FD. The controller advertises a data-phase timing table
    // whether or not FD is switched on, which is what makes it the right
    // signal for "can this do FD".
    check(state->fdCapable, "the captured PCAN-USB Pro FD reads as FD-capable");
    check(state->fdEnabled.has_value() && !*state->fdEnabled,
          "FD is not enabled on the captured interface");
    check(state->listenOnly.has_value() && !*state->listenOnly,
          "listen-only is off on the captured interface");
    check(state->state.has_value() && *state->state == CanState::Stopped,
          "a down controller reads as stopped");
    check(state->rxErrorCounter.value_or(0xFFFF) == 0, "the captured rx error counter is zero");
    check(state->txErrorCounter.value_or(0xFFFF) == 0, "the captured tx error counter is zero");
    check(state->busOffCount.has_value() && *state->busOffCount == 0,
          "the captured bus-off count is zero");
}

// Everything the captured reply cannot show, because the interface behind it
// could not be brought up on the machine that captured it.
void test_running_interface_decoding()
{
    auto message = ReplyBuilder {}
                       .up(true)
                       .nominal(500000, 875)
                       .data(2000000, 800)
                       .fd_capable()
                       // CAN_CTRLMODE_FD | CAN_CTRLMODE_LISTENONLY, both set.
                       .ctrlmode(0x3F, kCanCtrlModeFd | kCanCtrlModeListenOnly)
                       .state(1) // CAN_STATE_ERROR_WARNING
                       .berr(17, 42)
                       .device_stats(3, 2)
                       .build();

    auto state = decode_link_state(message);
    check(state.has_value(), "a running interface's reply decodes");
    if (!state.has_value())
    {
        return;
    }

    check(state->up, "IFF_UP reads as up");
    check(state->nominalBps.value_or(0) == 500000, "the nominal bit rate is read back");
    check(state->nominalSamplePointPermille.value_or(0) == 875, "the sample point is read back");
    check(state->dataBps.value_or(0) == 2000000, "the data bit rate is read back");
    check(state->fdEnabled.value_or(false), "CAN_CTRLMODE_FD reads as FD enabled");
    check(state->listenOnly.value_or(false), "CAN_CTRLMODE_LISTENONLY reads as listen-only");
    check(state->state.has_value() && *state->state == CanState::ErrorWarning,
          "CAN_STATE_ERROR_WARNING is read back");
    // struct can_berr_counter is tx first. Reading them the other way round
    // gives two plausible numbers that blame the wrong end of the bus.
    check(state->txErrorCounter.value_or(0) == 17, "the tx error counter comes first");
    check(state->rxErrorCounter.value_or(0) == 42, "the rx error counter comes second");
    check(state->busOffCount.value_or(0) == 3, "bus-off count comes from the xstats block");
    check(state->restartCount.value_or(0) == 2, "restart count comes from the xstats block");
}

void test_zero_bitrate_is_unset()
{
    // A bittiming attribute that is present but carries zero means the
    // controller has never been timed. Reporting that as "0 bit/s" would make
    // an unconfigured interface look like a configured one with an absurd rate.
    auto message = ReplyBuilder {}.up(false).nominal(0, 0).build();
    auto state = decode_link_state(message);
    check(state.has_value(), "a zero bittiming still decodes");
    check(state.has_value() && !state->nominalBps.has_value(),
          "a zero bit rate reads as unset rather than as zero");
}

void test_link_state_rejections()
{
    // Truncated before the headers are complete.
    check(!decode_link_state(std::span<const uint8_t>()).has_value(), "an empty reply is refused");
    check(!decode_link_state(std::span(golden::kCan0DownReply, size_t { 20 })).has_value(),
          "a reply shorter than nlmsghdr + ifinfomsg is refused");

    // The kernel's refusal, which arrives as an NLMSG_ERROR rather than a link.
    std::vector<uint8_t> error(24, 0);
    error[0] = 24;
    error[4] = 2; // NLMSG_ERROR
    error[16] = 0xFF;
    error[17] = 0xFF;
    error[18] = 0xFF;
    error[19] = 0xFF; // -1
    check(!decode_link_state(error).has_value(), "an NLMSG_ERROR reply is refused");

    // A link that is not a CAN interface. Decoding it would report an ethernet
    // card as a bus that is up and has never gone bus-off.
    ReplyBuilder ethernet;
    ethernet.kind = "veth";
    ethernet.up(true);
    check(!decode_link_state(ethernet.build()).has_value(), "a non-CAN link reply is refused");

    // A declared length past the end of what arrived.
    auto truncated = ReplyBuilder {}.up(true).nominal(500000, 875).build();
    truncated[0] = 0xFF;
    truncated[1] = 0xFF;
    check(!decode_link_state(truncated).has_value(),
          "a reply claiming more bytes than arrived is refused");

    // An attribute inside the CAN block whose length runs off the end. The
    // walk has to stop at it rather than read past it, and what was decoded
    // before it has to survive -- dropping the whole reply because its last
    // attribute was malformed would lose a bit rate that was read correctly.
    {
        ReplyBuilder builder;
        builder.up(true).nominal(500000, 875).fd_capable();
        // A trailing attribute claiming far more payload than remains.
        ReplyBuilder::attribute(builder.canAttributes, kIflaCanBerrCounter, { 1, 0, 2, 0 });
        auto overrun = builder.build();
        overrun[overrun.size() - 8] = 0xF0;
        overrun[overrun.size() - 7] = 0xFF;

        auto walked = decode_link_state(overrun);
        check(walked.has_value(), "a reply with an over-long trailing attribute still decodes");
        check(walked.has_value() && walked->nominalBps.value_or(0) == 500000,
              "the attributes before an over-long one are kept");
        check(walked.has_value() && walked->fdCapable,
              "and so is the FD capability that preceded it");
    }

    // An attribute claiming a length shorter than its own 4-byte header. This
    // is the one that hangs rather than misreads: align4(0) is 0, so a walk
    // that does not reject it never advances its offset. The assertion is that
    // this call RETURNS -- if the guard goes, the test times out instead of
    // failing, which is still a red run.
    {
        auto zeroLength = ReplyBuilder {}.up(true).nominal(500000, 875).fd_capable().build();
        // Blank the length of the link-info attribute in the top-level run.
        for (size_t i = 32; i + 4 <= zeroLength.size(); i += 4)
        {
            if (get_u16(zeroLength, i + 2) == kIflaLinkInfo)
            {
                zeroLength[i] = 0;
                zeroLength[i + 1] = 0;
                break;
            }
        }
        auto stopped = decode_link_state(zeroLength);
        // The walk stops before reaching any link info, so no CAN kind is ever
        // seen and the reply is refused rather than half-read.
        check(!stopped.has_value(), "a zero-length attribute is refused, not looped on");
        check(!stopped.has_value() || !stopped->fdCapable,
              "and nothing past it is decoded");
    }
}

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_classic_frame_round_trip();
    test_extended_identifier();
    test_error_frames();
    test_fd_frames();
    test_remote_frames();
    test_bad_reads_are_rejected();

    test_interface_name_validation();
    test_link_request_structure();
    test_bittiming_encoding();
    test_link_request_rejections();
    test_ack_decoding();

    test_availability_is_honest();

    test_link_query_structure();
    test_link_query_rejections();
    test_real_down_interface_capture();
    test_running_interface_decoding();
    test_zero_bitrate_is_unset();
    test_link_state_rejections();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all SocketCAN checks passed");
    return 0;
}
