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

#include "can_socketcan/netlink.h"
#include "can_socketcan/socketcan_backend.h"
#include "can_socketcan/socketcan_frame.h"

#include "can/bitrate.h"

#include <spdlog/spdlog.h>

#include <string>

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

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all SocketCAN checks passed");
    return 0;
}
