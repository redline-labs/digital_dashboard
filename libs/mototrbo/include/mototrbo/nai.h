// SPDX-License-Identifier: GPL-3.0-or-later
//
// MOTOTRBO's Network Application Interface: the data services.
//
// These are NOT XNL. They ride UDP straight on the radio's IP link, one
// well-known port per service, and they are what carries text messages,
// location reports and registration. The codecs live here; libs/xpr owns the
// socket that carries them.
//
// PROVENANCE, AND WHAT IT MEANS FOR TRUSTING THIS FILE. The ports and formats
// are corroborated across two independent community implementations,
// node-dmr-lib (rick51231) and Moto.Net (pboyd04). Only TMS has been exercised
// against a real XPR 5550, and it works. LRRP got no response to any of nine request framings on
// that radio, most likely because GPS is not enabled in its codeplug -- so the
// LRRP side below is UNPROVEN, and its coordinate decode is arithmetic checked
// against the documented scale rather than against a fix. ARS is untested: it
// needs the radio configured to register against this host and power-cycled.
//
// Two of the ports are worth noting for a different reason: 8002 and 8003
// appear in the community port list and match the XNL control ports we
// recovered independently, which is a small cross-check that the two bodies of
// work describe the same radio.

#ifndef MOTOTRBO_NAI_H
#define MOTOTRBO_NAI_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mototrbo/byte_order.h"
#include "mototrbo/error.h"

namespace mototrbo::nai
{

// Well-known ports, radio side.
inline constexpr std::uint16_t kPortDdms = 3000; // presence (TCP)
inline constexpr std::uint16_t kPortLrrp = 4001; // location
inline constexpr std::uint16_t kPortArs  = 4005; // registration
inline constexpr std::uint16_t kPortTms  = 4007; // text messaging
inline constexpr std::uint16_t kPortTelemetry = 4008;
inline constexpr std::uint16_t kPortOta  = 4009; // over-the-air programming
inline constexpr std::uint16_t kPortBms  = 4012; // battery management
inline constexpr std::uint16_t kPortJms  = 4013; // job tickets
inline constexpr std::uint16_t kPortSrr  = 4015; // sensor request/response
inline constexpr std::uint16_t kPortLip  = 5017; // indoor location
inline constexpr std::uint16_t kPortXnl  = 8002; // XNL control (TCP)
inline constexpr std::uint16_t kPortXnlSecure = 8003;

// ---- ARS: Automatic Registration Service, port 4005 -----------------------
namespace ars
{

enum class MessageType : std::uint8_t
{
    DeviceRegistration   = 0x0,
    DeviceDeregistration = 0x1,
    NameResolution       = 0x2,
    AddressResolution    = 0x3,
    DeviceQuery          = 0x4,
    UserRegistration     = 0x5,
    UserDeregistration   = 0x6,
    UserRegistrationAck  = 0x7,
    Keepalive            = 0x8,
    RegistrationAck      = 0xF,
};

// The type is the low nibble of byte 2.
constexpr Result<MessageType> message_type(std::span<const std::uint8_t> datagram)
{
    if (datagram.size() < 3)
    {
        return truncated(static_cast<std::uint16_t>(datagram.size()));
    }

    return static_cast<MessageType>(datagram[2] & 0x0Fu);
}

} // namespace ars

// ---- LRRP: Location Request/Response Protocol, port 4001 ------------------
namespace lrrp
{

enum class PacketType : std::uint8_t
{
    ImmediateLocationRequest  = 0x05,
    ImmediateLocationResponse = 0x07,
    TriggeredStartRequest     = 0x09,
    TriggeredStartResponse    = 0x0B,
    TriggeredLocationData     = 0x0D,
    TriggeredStopRequest      = 0x0F,
    TriggeredStopResponse     = 0x11,
    ProtocolVersionRequest    = 0x14,
    ProtocolVersionResponse   = 0x15,
};

// Tokens inside a response. A subset; the ones that carry a position are the
// ones this build looks for.
enum class Token : std::uint8_t
{
    Id          = 0x22,
    Time        = 0x34,
    StatusCode  = 0x37,
    StatusOk    = 0x38,
    Location3d  = 0x51,
    Direction   = 0x56,
    Location    = 0x66,
    LocationAlt = 0x69,
    Speed       = 0x6C,
};

// A coordinate is a signed 32-bit fraction of a full turn:
//   latitude  = raw * 180 / 2^32
//   longitude = raw * 360 / 2^32
constexpr double decode_latitude(std::int32_t raw)
{
    return static_cast<double>(raw) * (180.0 / 4294967296.0);
}

constexpr double decode_longitude(std::int32_t raw)
{
    return static_cast<double>(raw) * (360.0 / 4294967296.0);
}

struct Position
{
    double latitudeDeg { 0.0 };
    double longitudeDeg { 0.0 };
};

// Scan a response for the first location token and decode the pair after it.
//
// A SCAN, NOT A PARSE, and the difference is the point: the token stream's
// grammar is not modelled here, so this can match a byte sequence that merely
// looks like a location token. It is a best effort over an unproven format --
// see the provenance note at the top of the file -- and a hit should be
// treated as a hint until it can be checked against a radio with GPS enabled.
constexpr Result<Position> find_position(std::span<const std::uint8_t> datagram)
{
    for (std::size_t i = 0; i + 9 <= datagram.size(); ++i)
    {
        const std::uint8_t token = datagram[i];
        if (token != static_cast<std::uint8_t>(Token::Location) &&
            token != static_cast<std::uint8_t>(Token::Location3d) &&
            token != static_cast<std::uint8_t>(Token::LocationAlt))
        {
            continue;
        }

        return Position { decode_latitude(static_cast<std::int32_t>(read_u32(datagram, i + 1))),
                          decode_longitude(static_cast<std::int32_t>(read_u32(datagram, i + 5))) };
    }

    return truncated(static_cast<std::uint16_t>(datagram.size()));
}

} // namespace lrrp

// ---- TMS: Text Messaging Service, port 4007 -------------------------------
namespace tms
{

enum class MessageType : std::uint8_t
{
    SimpleText          = 0x00,
    ServiceAvailability = 0x10,
    Ack                 = 0x1F,
};

// Encode a simple text message. The body length is the UTF-16 byte count plus
// eight, and the text is UTF-16 LITTLE-endian -- the opposite of the display
// broadcasts in control.h, which is a trap worth having tripped over once.
//
// ASCII only. A non-ASCII character is refused rather than silently mangled
// into a wrong code unit, and text long enough to overflow the single-byte
// length field is refused too.
Result<std::vector<std::uint8_t>> encode_text(std::string_view text, std::uint8_t sequence = 0,
                                              bool requiresAck = false);

struct TextMessage
{
    MessageType type { MessageType::SimpleText };
    std::uint8_t sequence { 0 };
    std::string text;
};

Result<TextMessage> parse(std::span<const std::uint8_t> datagram);

} // namespace tms

} // namespace mototrbo::nai

#endif // MOTOTRBO_NAI_H
