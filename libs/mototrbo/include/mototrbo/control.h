// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading and steering a running radio: identity, status, the selected
// channel, and the broadcasts the radio pushes when its own state changes.
//
// All of it is XCMP inside an XNL data message, so nothing here owns a socket
// -- these are builders and parsers over bytes, and they are constexpr so the
// encodings can be asserted at build time against traffic a real XPR 5550
// accepted.
//
// STATUS, IN ONE PLACE. MOTOTRBO_STATUS_ITEM_TABLE is the single list of
// RadioStatus items: it generates the enum, the wire values, the names the
// node exposes, and how each item's bytes are read. The `supported` column
// records what an XPR 5550 mobile actually answers, from a sweep of items
// 0x00-0x7F -- several items community sources list are refused by a mobile,
// and a table that did not say so would have the node reporting a protocol
// error for a question this radio was never going to answer.
//
// WHAT IS NOT HERE. No PTT, no transmit, no RF tuning, no radio
// enable/disable/kill. See the note in xcmp.h.

#ifndef MOTOTRBO_CONTROL_H
#define MOTOTRBO_CONTROL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "mototrbo/byte_order.h"
#include "mototrbo/error.h"
#include "mototrbo/xcmp.h"

namespace mototrbo::control
{

// ===========================================================================
// Zone and channel (0x040D)  [CONFIRMED on hardware]
//
// The request is <op:1> followed by four bytes; the reply body (after the
// result code) is <op:1><u16><u16> with the op ECHOED -- and it is the op that
// was asked for, so it is 0x80 only for a query and 0x03/0x04 for a step.
// Keying a parser on 0x80 mis-reads every reply that is not a query.
// ===========================================================================

enum class ChannelOp : std::uint8_t
{
    Query      = 0x80, // -> 00 80 <zone:2> <ch:2>
    ZoneCount  = 0x81, // -> 00 81 <count:2> 0000
    ChannelCount = 0x82, // -> 00 82 0000 <count:2>, always for the current zone
    Up         = 0x03,
    Down       = 0x04,
    Select     = 0x07, // 07 <zone:2> <ch:2> -- see the caveat on select_zone_channel
};

constexpr xcmp::Command channel_up()
{
    return xcmp::Command(xcmp::Opcode::ZoneChannel, { static_cast<std::uint8_t>(ChannelOp::Up), 0, 0, 0, 0 });
}

constexpr xcmp::Command channel_down()
{
    return xcmp::Command(xcmp::Opcode::ZoneChannel, { static_cast<std::uint8_t>(ChannelOp::Down), 0, 0, 0, 0 });
}

constexpr xcmp::Command query_channel()
{
    return xcmp::Command(xcmp::Opcode::ZoneChannel, { static_cast<std::uint8_t>(ChannelOp::Query), 0, 0, 0, 0 });
}

constexpr xcmp::Command query_zone_count()
{
    return xcmp::Command(xcmp::Opcode::ZoneChannel, { static_cast<std::uint8_t>(ChannelOp::ZoneCount), 0, 0, 0, 0 });
}

// Its argument is ignored: the radio always answers for the zone it is on.
constexpr xcmp::Command query_channel_count()
{
    return xcmp::Command(xcmp::Opcode::ZoneChannel,
                         { static_cast<std::uint8_t>(ChannelOp::ChannelCount), 0, 0, 0, 0 });
}

// Direct select: 07 <zone:2> <channel:2>.
//
// TWO THINGS ABOUT THIS, both confirmed on an XPR 5550. The widely-cited
// `07 00 <zone:2> 00` framing is rejected outright with invalid_parameter; the
// framing below is the one the radio accepts. And even so, on this radio the
// operation only VALIDATES -- it returns success, echoes the unchanged zone
// and channel, and does so even for a channel that does not exist. Stepping
// with channel_up/channel_down is the only way to actually move, which is why
// xpr::Radio::selectChannel steps rather than sends this.
constexpr xcmp::Command select_zone_channel(std::uint16_t zone, std::uint16_t channel)
{
    return xcmp::Command(xcmp::Opcode::ZoneChannel,
                         { static_cast<std::uint8_t>(ChannelOp::Select),
                           static_cast<std::uint8_t>((zone >> 8) & 0xFFu),
                           static_cast<std::uint8_t>(zone & 0xFFu),
                           static_cast<std::uint8_t>((channel >> 8) & 0xFFu),
                           static_cast<std::uint8_t>(channel & 0xFFu) });
}

struct ChannelReply
{
    ChannelOp operation {};

    // For a query, a step, or a select echo these are where the radio now is.
    // For the counts only one is meaningful -- 0x81 answers in `zone`, 0x82 in
    // `channel` -- which is why they are two fields rather than one.
    std::uint16_t zone { 0 };
    std::uint16_t channel { 0 };
};

// Takes the reply BODY, i.e. what xcmp::reply_body() returns once the result
// code has been checked and stripped: <op:1><u16><u16>.
constexpr Result<ChannelReply> parse_channel_reply(std::span<const std::uint8_t> body)
{
    if (body.size() < 5)
    {
        return truncated(static_cast<std::uint16_t>(body.size()));
    }

    return ChannelReply { static_cast<ChannelOp>(body[0]), read_u16(body, 1), read_u16(body, 3) };
}

struct ZoneChannel
{
    std::uint16_t zone { 0 };
    std::uint16_t channel { 0 };
};

constexpr bool operator==(const ZoneChannel& a, const ZoneChannel& b)
{
    return a.zone == b.zone && a.channel == b.channel;
}

// The 0xB40D broadcast has no result code and no echoed op: it is just
// <zone:2><ch:2>, sometimes with one trailing byte.
constexpr Result<ZoneChannel> parse_channel_broadcast(std::span<const std::uint8_t> payload)
{
    if (payload.size() < 4)
    {
        return truncated(static_cast<std::uint16_t>(payload.size()));
    }

    return ZoneChannel { read_u16(payload, 0), read_u16(payload, 2) };
}

// ===========================================================================
// Radio status (0x000E), version, datecode, TANAPA
// ===========================================================================

enum class StatusEncoding : std::uint8_t
{
    // NUL-padded ASCII.
    Ascii,
    // A big-endian unsigned integer, however many bytes the radio sent.
    Unsigned,
    // Bytes whose meaning we do not know. Reported as bytes rather than
    // guessed at.
    Opaque,
};

// item, wire value, exposed name, encoding, answered by an XPR 5550 mobile
//
// The `false` rows are items community sources list which this radio refuses
// with a non-zero result: every station/RDAC meter, the product serial, the
// alias and the uptime. They are kept because a caller asking for one deserves
// "this radio does not have it" rather than a protocol error.
#define MOTOTRBO_STATUS_ITEM_TABLE(X)                                                   \
    X(Rssi,           0x02, "rssi",            Unsigned, true)                          \
    X(ModelNumber,    0x07, "model_number",    Ascii,    true)                           \
    X(SerialNumber,   0x08, "serial_number",   Ascii,    true)                           \
    X(Esn,            0x09, "esn",             Opaque,   true)                           \
    X(ProductSerial,  0x0B, "product_serial",  Ascii,    false)                          \
    X(SignalingMode,  0x0D, "signaling_mode",  Unsigned, true)                           \
    X(RadioId,        0x0E, "radio_id",        Unsigned, true)                           \
    X(RadioAlias,     0x0F, "radio_alias",     Ascii,    false)                          \
    X(BatteryVoltage, 0x33, "battery_voltage", Unsigned, false)                          \
    X(PaTemperature,  0x41, "pa_temperature",  Unsigned, false)                          \
    X(OutputPower,    0x42, "output_power",    Unsigned, false)                          \
    X(Vswr,           0x43, "vswr",            Unsigned, false)                          \
    X(Uptime,         0x4D, "uptime",          Unsigned, false)

enum class StatusItem : std::uint8_t
{
#define MOTOTRBO_STATUS_ENUMERATOR(name, value, text, encoding, supported) name = value,
    MOTOTRBO_STATUS_ITEM_TABLE(MOTOTRBO_STATUS_ENUMERATOR)
#undef MOTOTRBO_STATUS_ENUMERATOR
};

struct StatusItemSpec
{
    StatusItem item {};
    std::string_view name;
    StatusEncoding encoding { StatusEncoding::Opaque };

    // Answered by the XPR 5550 mobile this was validated against. False means
    // the radio returns a non-zero result for it.
    bool supported { false };
};

inline constexpr auto kStatusItems = [] {
#define MOTOTRBO_STATUS_COUNT(name, value, text, encoding, supported) +1
    constexpr std::size_t count = 0 MOTOTRBO_STATUS_ITEM_TABLE(MOTOTRBO_STATUS_COUNT);
#undef MOTOTRBO_STATUS_COUNT

    return std::array<StatusItemSpec, count> { {
#define MOTOTRBO_STATUS_ROW(name, value, text, encoding, supported) \
    StatusItemSpec { StatusItem::name, text, StatusEncoding::encoding, supported },
        MOTOTRBO_STATUS_ITEM_TABLE(MOTOTRBO_STATUS_ROW)
#undef MOTOTRBO_STATUS_ROW
    } };
}();

constexpr const StatusItemSpec* find_status_item(StatusItem item)
{
    for (const StatusItemSpec& spec : kStatusItems)
    {
        if (spec.item == item)
        {
            return &spec;
        }
    }

    return nullptr;
}

constexpr const StatusItemSpec* find_status_item(std::string_view name)
{
    for (const StatusItemSpec& spec : kStatusItems)
    {
        if (spec.name == name)
        {
            return &spec;
        }
    }

    return nullptr;
}

constexpr xcmp::Command radio_status(StatusItem item)
{
    return xcmp::Command(xcmp::Opcode::RadioStatus, { static_cast<std::uint8_t>(item) });
}

// VersionInformation (0x000F) takes a request-type byte: 0x00 host software,
// 0x42 codeplug, 0x63 RF band, 0x65 power, 0x6B ESN, 0x80/0x81 option board.
// The reply body is ASCII.
inline constexpr std::uint8_t kVersionHostSoftware = 0x00;

constexpr xcmp::Command version_info(std::uint8_t requestType = kVersionHostSoftware)
{
    return xcmp::Command(xcmp::Opcode::VersionInfo, { requestType });
}

// Both of these refuse an empty payload with invalid_parameter; the argument
// widths below are the ones the radio accepts. [CONFIRMED on hardware]
constexpr xcmp::Command datecode()
{
    return xcmp::Command(xcmp::Opcode::Datecode, { 0x00 });
}

constexpr xcmp::Command tanapa_number()
{
    return xcmp::Command(xcmp::Opcode::TanapaNumber, { 0x00, 0x00 });
}

struct StatusReading
{
    StatusItem item {};
    // A VIEW into the reply buffer.
    std::span<const std::uint8_t> value;
};

// The RadioStatus reply body is <item:1><value...> -- the item is ECHOED.
// [CONFIRMED from the vendor client's own decoder, which reads a byte and then
// a byte array from offset 3 -- the opcode plus the result code.]
//
// `requested` is checked rather than assumed. If the echo does not match, this
// reports UnexpectedOpcode instead of returning a value read one byte off --
// which is the failure mode a wrong layout would otherwise have, and it is
// silent.
constexpr Result<StatusReading> parse_status(StatusItem requested, std::span<const std::uint8_t> body)
{
    if (body.empty())
    {
        return truncated(0);
    }
    if (body[0] != static_cast<std::uint8_t>(requested))
    {
        return unexpected_opcode(body[0]);
    }

    return StatusReading { requested, body.subspan(1) };
}

// A big-endian unsigned value of whatever width the radio sent, up to four
// bytes. Longer replies are refused rather than truncated: a five-byte value
// where a counter was expected means the item is not what we think it is.
constexpr Result<std::uint32_t> status_unsigned(std::span<const std::uint8_t> value)
{
    if (value.empty() || value.size() > 4)
    {
        return length_mismatch(0);
    }

    std::uint32_t out = 0;
    for (const std::uint8_t byte : value)
    {
        out = (out << 8) | byte;
    }

    return out;
}

// NUL-padded ASCII, as a view into the reply buffer. Used for status items
// and for the whole body of the version, TANAPA and model replies, which are
// ASCII outright.
//
// Not constexpr: getting from a byte span to a string_view needs a
// reinterpret_cast, which constant evaluation does not allow.
inline std::string_view ascii_text(std::span<const std::uint8_t> value)
{
    std::size_t length = 0;
    while (length < value.size() && value[length] != 0)
    {
        ++length;
    }

    return std::string_view(reinterpret_cast<const char*>(value.data()), length);
}

// ===========================================================================
// Unsolicited broadcasts (0xB4xx)
//
// The radio pushes these whenever its own state changes, with no request
// involved, interleaved with command replies. Two of the seven an XPR 5550
// emits are decoded; the rest are named and passed through as bytes.
//
// The five that are not decoded are not decoded for a reason worth stating:
// three of them never changed value across a capture that included six channel
// changes, and you cannot infer a field's meaning from a constant. Decoding
// them needs a session that provokes the state they report -- a call, a volume
// change, a low battery -- not more staring at these bytes.
// ===========================================================================

enum class BroadcastKind : std::uint16_t
{
    RadioStatus = 0xB400, // periodic status dump, item/value pairs
    DisplayText = 0xB401, // one line of the radio's own display     -- decoded
    RadioEvent  = 0xB402, // state/event notification, ids unknown
    Meter       = 0xB406, // 4 bytes, constant in every capture
    CallState   = 0xB407, // 4 bytes, constant in every capture
    ZoneChannel = 0xB40D, // the selected channel changed            -- decoded
    Capability  = 0xB41C, // capability/mode announcement
};

constexpr bool is_broadcast(std::uint16_t opcode)
{
    return (opcode & xcmp::kBroadcastMask) == xcmp::kBroadcastPrefix;
}

constexpr std::string_view broadcast_name(std::uint16_t opcode)
{
    constexpr std::array<std::pair<BroadcastKind, std::string_view>, 7> kNames { {
        { BroadcastKind::RadioStatus, "status" },
        { BroadcastKind::DisplayText, "display" },
        { BroadcastKind::RadioEvent, "event" },
        { BroadcastKind::Meter, "meter" },
        { BroadcastKind::CallState, "call-state" },
        { BroadcastKind::ZoneChannel, "zone/channel" },
        { BroadcastKind::Capability, "capability" },
    } };

    for (const auto& [kind, name] : kNames)
    {
        if (static_cast<std::uint16_t>(kind) == opcode)
        {
            return name;
        }
    }

    return "unknown";
}

// One line of the radio's display, as pushed in a 0xB401 broadcast.
//
//     <line:1> <flags:1> <textlen:2> <UTF-16BE text with ANSI CSI escapes>
//
// The length field matched the payload in 60 of 60 captured frames. The XPR
// 5550 has four lines and line 4 is the softkey row, whose labels are
// separated by U+EFCD.
struct DisplayLine
{
    std::uint8_t line { 0 };
    std::uint8_t flags { 0 };
    // Still UTF-16BE with the escapes in it -- decode_display_text() turns it
    // into something printable. A VIEW into the reply buffer.
    std::span<const std::uint8_t> encodedText;
};

struct Broadcast
{
    std::uint16_t opcode { 0 };
    // Everything after the 2-byte opcode. A VIEW into the reply buffer.
    std::span<const std::uint8_t> payload;

    std::optional<DisplayLine> display;      // 0xB401
    std::optional<ZoneChannel> zoneChannel;  // 0xB40D
};

constexpr Result<Broadcast> parse_broadcast(const xcmp::Message& message)
{
    if (!message.isBroadcast())
    {
        return unexpected_opcode(message.opcode);
    }

    Broadcast out;
    out.opcode = message.opcode;
    out.payload = message.payload;

    if (message.opcode == static_cast<std::uint16_t>(BroadcastKind::DisplayText))
    {
        if (out.payload.size() >= 4)
        {
            const std::uint16_t declared = read_u16(out.payload, 2);
            const std::size_t available = out.payload.size() - 4;
            DisplayLine line;
            line.line = out.payload[0];
            line.flags = out.payload[1];
            line.encodedText = out.payload.subspan(4, declared < available ? declared : available);
            out.display = line;
        }
    }
    else if (message.opcode == static_cast<std::uint16_t>(BroadcastKind::ZoneChannel))
    {
        if (const Result<ZoneChannel> zc = parse_channel_broadcast(out.payload); zc.has_value())
        {
            out.zoneChannel = *zc;
        }
    }

    return out;
}

// Motorola's on-wire display text -- UTF-16 BIG-endian with ANSI CSI escape
// sequences -- as printable UTF-8, escapes removed and the softkey separator
// rendered as " | ".
//
// NOTE THE ENDIANNESS. Codeplug strings are UTF-16 LITTLE-endian; the display
// path is the other way round. Reading this one little-endian produces CJK
// mojibake rather than an error.
//
// The only function in this library that allocates, which is why it is not
// constexpr and why the parsers above hand back the encoded bytes instead of
// calling it themselves.
std::string decode_display_text(std::span<const std::uint8_t> utf16be);

} // namespace mototrbo::control

#endif // MOTOTRBO_CONTROL_H
