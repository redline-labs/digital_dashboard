// SPDX-License-Identifier: GPL-3.0-or-later
//
// Channel control, radio status, and the broadcasts the radio pushes on its
// own -- checked against bytes an XPR 5550 sent or accepted.
//
// The command encodings and the byte-level parsers are asserted at COMPILE
// time; the display-text decode is the exception, because turning UTF-16BE
// with ANSI escapes into UTF-8 allocates.
//
// Two cases here are worth more than the rest, both because they are wrong
// answers rather than failures:
//
//   * A 0x840D reply echoes the operation that was ASKED FOR, not always 0x80.
//     A parser keyed on 0x80 mis-reads every reply to a channel step -- and
//     returns a plausible zone and channel while doing it.
//   * The display broadcasts are UTF-16 BIG-endian while codeplug strings are
//     little-endian. Reading this path the other way round gives CJK mojibake,
//     not an error.

#include "mototrbo/control.h"
#include "mototrbo/xcmp.h"

#include "golden/hardware_vectors.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <span>
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

using namespace mototrbo;

// ===========================================================================
// Compile time: the command encodings
//
// Every one of these is a byte sequence the radio accepted.
// ===========================================================================

constexpr bool encodes(const xcmp::Command& command, std::span<const std::uint8_t> expected)
{
    const std::span<const std::uint8_t> actual = command.bytes();
    if (actual.size() != expected.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        if (actual[i] != expected[i])
        {
            return false;
        }
    }

    return true;
}

static_assert(encodes(control::channel_up(), golden::hex("040d0300000000")));
static_assert(encodes(control::channel_down(), golden::hex("040d0400000000")));
static_assert(encodes(control::query_channel(), golden::hex("040d8000000000")));
static_assert(encodes(control::query_zone_count(), golden::hex("040d8100000000")));
static_assert(encodes(control::query_channel_count(), golden::hex("040d8200000000")));

// 07 <zone:2> <channel:2>. The widely-cited `07 00 <zone:2> 00` form is
// rejected outright by the radio with invalid_parameter.
static_assert(encodes(control::select_zone_channel(3, 5), golden::hex("040d0700030005")));

static_assert(encodes(control::radio_status(control::StatusItem::Rssi), golden::hex("000e02")));
static_assert(encodes(control::radio_status(control::StatusItem::RadioId), golden::hex("000e0e")));
static_assert(encodes(control::version_info(), golden::hex("000f00")));

// Both of these refuse an empty payload; the widths below are what the radio
// accepts.
static_assert(encodes(control::datecode(), golden::hex("001900")));
static_assert(encodes(control::tanapa_number(), golden::hex("001f0000")));

// A reply carries the request's opcode with bit 15 set.
static_assert(control::query_channel().replyOpcode() == 0x840D);

// ===========================================================================
// Compile time: parsing what came back
// ===========================================================================

// The whole path a reply takes: message, result code, body, fields.
constexpr Result<control::ChannelReply> parseChannelReply(std::span<const std::uint8_t> message)
{
    const Result<xcmp::Message> parsed = xcmp::parse_message(message);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }

    const Result<std::span<const std::uint8_t>> body = xcmp::reply_body(*parsed);
    if (!body.has_value())
    {
        return std::unexpected(body.error());
    }

    return control::parse_channel_reply(*body);
}

static_assert(parseChannelReply(golden::kChannelQueryReply)->operation == control::ChannelOp::Query);
static_assert(parseChannelReply(golden::kChannelQueryReply)->zone == 1);
static_assert(parseChannelReply(golden::kChannelQueryReply)->channel == 2);

// The echoed operation is the one asked for. A parser that skipped a fixed
// 0x80 would read zone 0x0001 as 0x0100 here and report zone 256.
static_assert(parseChannelReply(golden::kChannelUpReply)->operation == control::ChannelOp::Up);
static_assert(parseChannelReply(golden::kChannelUpReply)->zone == 1);
static_assert(parseChannelReply(golden::kChannelUpReply)->channel == 3);
static_assert(parseChannelReply(golden::kChannelDownReply)->operation == control::ChannelOp::Down);
static_assert(parseChannelReply(golden::kChannelDownReply)->channel == 2);

// A refusal is an error carrying the radio's own result code, not a value.
// `00 0e 04 ...` is invalid_parameter, which is what an out-of-range channel
// index gets.
static_assert(parseChannelReply(golden::hex("840d04800001000200")).error().kind == ErrorKind::DeviceNak);
static_assert(parseChannelReply(golden::hex("840d04800001000200")).error().detail ==
              static_cast<std::uint8_t>(xcmp::ResultCode::InvalidParameter));

// A reply body one byte short of a zone/channel pair.
static_assert(parseChannelReply(golden::hex("840d00800001")).error().kind == ErrorKind::Truncated);

// The broadcast has neither result code nor echoed operation, and comes in two
// lengths.
constexpr Result<control::Broadcast> parseBroadcast(std::span<const std::uint8_t> message)
{
    const Result<xcmp::Message> parsed = xcmp::parse_message(message);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }

    return control::parse_broadcast(*parsed);
}

static_assert(parseBroadcast(golden::kChannelBroadcast4)->zoneChannel == control::ZoneChannel { 1, 3 });
static_assert(parseBroadcast(golden::kChannelBroadcast5)->zoneChannel == control::ZoneChannel { 1, 2 });

// A broadcast this build does not model still parses, keeps its bytes, and is
// named -- a radio saying something new must not look like a silent one.
static_assert(parseBroadcast(golden::kEventBroadcast)->opcode == 0xB402);
static_assert(parseBroadcast(golden::kEventBroadcast)->payload.size() == 12);
static_assert(!parseBroadcast(golden::kEventBroadcast)->display.has_value());
static_assert(control::broadcast_name(0xB402) == "event");
static_assert(control::broadcast_name(0xB4FF) == "unknown");

// A command reply is not a broadcast, and handing one to the broadcast parser
// is an error rather than a silently empty result.
static_assert(!control::is_broadcast(0x840D));
static_assert(parseBroadcast(golden::kChannelQueryReply).error().kind == ErrorKind::UnexpectedOpcode);

// ---- radio status ---------------------------------------------------------
// The reply body is <item:1><value...>: the item is echoed. That layout comes
// from the vendor client's own decoder, which reads a byte and then a byte
// array from offset 3 (the opcode plus the result code) -- but no reply to a
// status query was captured byte for byte, so the two vectors below are
// SYNTHETIC: real values (this radio's DMR id 9176 and its model number)
// wrapped in that framing. If the echo is ever wrong on hardware, parse_status
// reports it rather than returning a value read one byte off, which is the
// whole reason it checks.
static_assert(control::parse_status(control::StatusItem::RadioId, golden::hex("0e000023d8"))->value.size() == 4);
static_assert(control::status_unsigned(
                  control::parse_status(control::StatusItem::RadioId, golden::hex("0e000023d8"))->value)
                  .value() == 9176);

// Asking for one item and being answered about another is an error, not a
// value. 9176 is this radio's DMR id, and it appears in no readable codeplug
// item under any encoding -- this query is the only way to learn it.
static_assert(control::parse_status(control::StatusItem::Rssi, golden::hex("0e000023d8")).error().kind ==
              ErrorKind::UnexpectedOpcode);

static_assert(control::status_unsigned(golden::hex("0102030405")).error().kind == ErrorKind::LengthMismatch);
static_assert(control::status_unsigned(std::span<const std::uint8_t> {}).error().kind ==
              ErrorKind::LengthMismatch);

// The item table is the one place items are declared, and it records which
// ones this radio actually answers -- several that community sources list are
// refused by a mobile.
static_assert(control::find_status_item("model_number")->item == control::StatusItem::ModelNumber);
static_assert(control::find_status_item("model_number")->encoding == control::StatusEncoding::Ascii);
static_assert(control::find_status_item("model_number")->supported);
static_assert(!control::find_status_item("uptime")->supported);
static_assert(!control::find_status_item("battery_voltage")->supported);
static_assert(control::find_status_item("no_such_item") == nullptr);
static_assert(control::find_status_item(control::StatusItem::Esn)->encoding == control::StatusEncoding::Opaque);

// ===========================================================================
// Run time: the display
// ===========================================================================

void checkDisplayBroadcasts()
{
    const Result<control::Broadcast> line3 = parseBroadcast(golden::kDisplayLine3);
    check(line3.has_value() && line3->display.has_value(), "display line 3 parses");
    if (line3.has_value() && line3->display.has_value())
    {
        check(line3->display->line == 3, "display line number");
        check(control::decode_display_text(line3->display->encodedText) == "3 Talkaround",
              "display text, escapes stripped");
    }

    const Result<control::Broadcast> line4 = parseBroadcast(golden::kDisplayLine4);
    check(line4.has_value() && line4->display.has_value(), "display line 4 parses");
    if (line4.has_value() && line4->display.has_value())
    {
        check(line4->display->line == 4, "softkey row is line 4");
        // U+EFCD separates the softkey labels.
        check(control::decode_display_text(line4->display->encodedText) == "OT-1 | OT-2 | OT-3 | Zn-s",
              "softkey labels");
    }

    // A display broadcast whose declared text length is longer than the bytes
    // that arrived must not read past them.
    const auto truncatedDisplay = golden::hex("b401030100900041");
    const Result<control::Broadcast> partial = parseBroadcast(truncatedDisplay);
    check(partial.has_value() && partial->display.has_value(), "a truncated display line still parses");
    if (partial.has_value() && partial->display.has_value())
    {
        check(partial->display->encodedText.size() == 2, "text is clamped to what arrived");
        check(control::decode_display_text(partial->display->encodedText) == "A", "clamped text decodes");
    }

    // Nothing but escapes and padding decodes to nothing, rather than to a
    // string of control characters.
    check(control::decode_display_text(golden::hex("001b005b0032004a0000")).empty(),
          "escapes and padding alone decode to nothing");
}

void checkStatusText()
{
    // The model number as this radio reports it, NUL-padded. Synthetic
    // framing around a real string -- see the note above.
    const auto reply = golden::hex("074d323854524e39574131414e0000");
    const Result<control::StatusReading> reading =
        control::parse_status(control::StatusItem::ModelNumber, reply);
    check(reading.has_value(), "model number status parses");
    if (reading.has_value())
    {
        check(control::ascii_text(reading->value) == "M28TRN9WA1AN", "model number text");
    }
}

} // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    checkDisplayBroadcasts();
    checkStatusText();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }

    SPDLOG_INFO("mototrbo_test_control passed");
    return 0;
}
