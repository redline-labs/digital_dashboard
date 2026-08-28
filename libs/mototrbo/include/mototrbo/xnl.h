// SPDX-License-Identifier: GPL-3.0-or-later
//
// XNL: the session and authentication layer a MOTOTRBO radio speaks over IP.
//
// The radio presents itself as a USB-RNDIS network adapter, so the transport
// is an ordinary TCP socket (libs/xpr owns that). Inside it, every message is
// a length-prefixed XNL frame; the ones carrying commands hold an XCMP
// message as their payload.
//
// Everything here is bytes and arithmetic -- no sockets, no allocation, all
// constexpr -- so the frame layout and the authentication cipher are checked
// at build time against captured traffic. That matters more than usual here:
// the layout was recovered from a DLL and corrected twice, and a wrong offset
// in this header does not crash, it produces a frame the radio silently drops.
//
// PROVENANCE. Values marked [CONFIRMED] were recovered by reverse-engineering
// the vendor's own client and then exercised against an XPR 5550 (serial
// 511TVMG951, firmware R02.10.00.0001); [DERIVED] ones follow the gaps and
// public descriptions of XNL and have not been seen on the wire.

#ifndef MOTOTRBO_XNL_H
#define MOTOTRBO_XNL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "mototrbo/byte_order.h"
#include "mototrbo/error.h"

namespace mototrbo::xnl
{

enum class Opcode : std::uint16_t
{
    MasterStatusBroadcast = 0x0002, // master -> device, announces the master address [DERIVED]
    DeviceMasterQuery     = 0x0003, // device -> master                               [CONFIRMED]
    DeviceAuthRequest     = 0x0004, // device -> master, asks for the challenge       [CONFIRMED]
    DeviceAuthReply       = 0x0005, // master -> device, carries the challenge        [CONFIRMED]
    DeviceConnRequest     = 0x0006, // device -> master, carries the auth response    [CONFIRMED]
    DeviceConnReply       = 0x0007, // master -> device, assigns our address          [DERIVED]
    DeviceSysmapRequest   = 0x0008, // device -> master                               [DERIVED]
    DeviceSysmapBroadcast = 0x0009, // master -> device                               [DERIVED]
    DataMessage           = 0x000B, // carries an XCMP payload                        [CONFIRMED]
    DataMessageAck        = 0x000C, // acknowledges a DataMessage                     [DERIVED]
};

constexpr const char* to_string(Opcode opcode)
{
    switch (opcode)
    {
        case Opcode::MasterStatusBroadcast: return "master status";
        case Opcode::DeviceMasterQuery:     return "master query";
        case Opcode::DeviceAuthRequest:     return "auth request";
        case Opcode::DeviceAuthReply:       return "auth reply";
        case Opcode::DeviceConnRequest:     return "conn request";
        case Opcode::DeviceConnReply:       return "conn reply";
        case Opcode::DeviceSysmapRequest:   return "sysmap request";
        case Opcode::DeviceSysmapBroadcast: return "sysmap broadcast";
        case Opcode::DataMessage:           return "data message";
        case Opcode::DataMessageAck:        return "data message ack";
    }

    return "unknown";
}

// ===========================================================================
// Authentication  [CONFIRMED: recovered from the vendor client, then proven
// against the radio]
//
// The master issues an 8-byte challenge; the device answers with the TEA
// encryption of it under a fixed 128-bit key. Note the delta: this is NOT the
// textbook 0x9E3779B9, and a build that used the textbook value would fail
// authentication with no diagnostic beyond an all-zero CONN_REPLY.
//
// That the answer is computed rather than replayed is proven on hardware: a
// deliberately wrong response is rejected by the radio.
// ===========================================================================

inline constexpr std::uint32_t kTeaDelta = 0x790AB771u;
inline constexpr std::array<std::uint32_t, 4> kTeaKey = { 0x5A96301Du, 0x0CF2AA55u, 0xBF936CC6u,
                                                          0xBD5ECD5Bu };

// Device type announced in DEVICE_CONN_REQUEST and echoed by the master at
// CONN_REPLY+4. 0x0A is what the vendor client announces.
// [CONFIRMED on hardware]
inline constexpr std::uint8_t kDeviceType = 0x0A;

// Bit 3 of DEVICE_CONN_REQUEST's flags byte picks the delivery mode for the
// whole connection. [CONFIRMED on hardware]
//
//   set    unacknowledged. The radio neither ACKs our data messages nor
//          expects ACKs for its own. This is what the vendor client does, and
//          what we do.
//   clear  acknowledged. The radio expects a DATA_MSG_ACK for each message it
//          sends and retransmits five times when it does not get one -- which
//          desynchronises the reply stream so completely that every query
//          returns the PREVIOUS query's answer.
inline constexpr std::uint8_t kConnFlagUnacked = 0x08;

constexpr void tea_encrypt_block(std::uint32_t& v0, std::uint32_t& v1,
                                 const std::array<std::uint32_t, 4>& key = kTeaKey,
                                 std::uint32_t delta = kTeaDelta)
{
    std::uint32_t a = v0;
    std::uint32_t b = v1;
    std::uint32_t sum = 0;

    for (int round = 0; round < 32; ++round)
    {
        sum += delta;
        a += ((b << 4) + key[0]) ^ (b + sum) ^ ((b >> 5) + key[1]);
        b += ((a << 4) + key[2]) ^ (a + sum) ^ ((a >> 5) + key[3]);
    }

    v0 = a;
    v1 = b;
}

// The 8-byte block is two big-endian words, v0 || v1.
constexpr std::array<std::uint8_t, 8> auth_response(std::span<const std::uint8_t, 8> challenge)
{
    std::uint32_t v0 = read_u32(challenge, 0);
    std::uint32_t v1 = read_u32(challenge, 4);
    tea_encrypt_block(v0, v1);

    std::array<std::uint8_t, 8> out {};
    write_u32(out, 0, v0);
    write_u32(out, 4, v1);
    return out;
}

// ===========================================================================
// The frame  [CONFIRMED layout -- from the vendor client's own serializer and
// parser, then exercised on hardware]
//
//   off size field
//    0   2   length        = 12 + payload_len, i.e. everything after this field
//    2   2   opcode
//    4   1   protocol      1 for XCMP data messages, 0 for handshake messages
//    5   1   flags         rolling 0..7 message counter
//    6   2   dst           recipient
//    8   2   src           sender
//   10   2   transaction   rolling, echoed in the reply
//   12   2   payload_len
//   14   ..  payload
//
// dst@6 before src@8 is confirmed three ways: our own reverse engineering plus
// two independent community implementations -- node-dmr-lib (rick51231) and
// Moto.Net (pboyd04) -- which agree. An earlier reading had them reversed.
// ===========================================================================

inline constexpr std::size_t kLengthPrefixSize = 2;
inline constexpr std::size_t kHeaderSize = 12; // everything the length field counts
inline constexpr std::size_t kFrameOverhead = kLengthPrefixSize + kHeaderSize;

struct Frame
{
    Opcode opcode {};
    std::uint8_t protocol { 0 };
    std::uint8_t flags { 0 };
    std::uint16_t dst { 0 };
    std::uint16_t src { 0 };
    std::uint16_t transaction { 0 };

    // A VIEW into the buffer the frame was parsed from, not a copy. It is
    // valid for exactly as long as that buffer holds these bytes -- see
    // xpr::Radio, which parses in place and hands the payload to the caller
    // before the next read overwrites it.
    std::span<const std::uint8_t> payload;
};

constexpr std::size_t frame_size(std::size_t payloadSize)
{
    return kFrameOverhead + payloadSize;
}

// How many bytes the frame starting at `raw` occupies, from its length field
// alone. This is what a stream reader needs before it has the whole frame.
constexpr Result<std::size_t> frame_length(std::span<const std::uint8_t> raw)
{
    if (raw.size() < kLengthPrefixSize)
    {
        return truncated(0);
    }

    return kLengthPrefixSize + read_u16(raw, 0);
}

constexpr Result<Frame> parse_frame(std::span<const std::uint8_t> raw)
{
    if (raw.size() < kFrameOverhead)
    {
        return truncated(static_cast<std::uint16_t>(raw.size()));
    }

    const std::uint16_t length = read_u16(raw, 0);
    if (length < kHeaderSize)
    {
        return length_mismatch(0);
    }
    if (raw.size() < kLengthPrefixSize + length)
    {
        return truncated(static_cast<std::uint16_t>(raw.size()));
    }

    const std::uint16_t payloadLength = read_u16(raw, 12);
    if (kHeaderSize + payloadLength > length)
    {
        return length_mismatch(12);
    }

    Frame frame;
    frame.opcode = static_cast<Opcode>(read_u16(raw, 2));
    frame.protocol = raw[4];
    frame.flags = raw[5];
    frame.dst = read_u16(raw, 6);
    frame.src = read_u16(raw, 8);
    frame.transaction = read_u16(raw, 10);
    frame.payload = raw.subspan(kFrameOverhead, payloadLength);
    return frame;
}

// Writes the frame into `out` and returns how many bytes it used. The caller
// owns the buffer, which is what keeps the send path free of allocation --
// every frame this library sends fits in a small stack array.
constexpr Result<std::size_t> serialize_frame(const Frame& frame, std::span<std::uint8_t> out)
{
    const std::size_t size = frame_size(frame.payload.size());
    if (out.size() < size)
    {
        return invalid_argument();
    }

    write_u16(out, 0, static_cast<std::uint16_t>(kHeaderSize + frame.payload.size()));
    write_u16(out, 2, static_cast<std::uint16_t>(frame.opcode));
    out[4] = frame.protocol;
    out[5] = frame.flags;
    write_u16(out, 6, frame.dst);
    write_u16(out, 8, frame.src);
    write_u16(out, 10, frame.transaction);
    write_u16(out, 12, static_cast<std::uint16_t>(frame.payload.size()));

    for (std::size_t i = 0; i < frame.payload.size(); ++i)
    {
        out[kFrameOverhead + i] = frame.payload[i];
    }

    return size;
}

// ---------------------------------------------------------------------------
// Handshake payloads
//
// Both of these are the shape of the bug that cost the most: a CONN_REQUEST
// with a 10-byte payload (address + response, the obvious reading) is silently
// DROPPED by the radio -- no reply, no error, nothing to debug. It is 12 bytes
// with the device type at offset 2 and the response at offset 4.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kConnRequestPayloadSize = 12;

constexpr std::array<std::uint8_t, kConnRequestPayloadSize>
conn_request_payload(std::span<const std::uint8_t, 8> challenge)
{
    std::array<std::uint8_t, kConnRequestPayloadSize> payload {};
    payload[2] = kDeviceType;

    const std::array<std::uint8_t, 8> response = auth_response(challenge);
    for (std::size_t i = 0; i < response.size(); ++i)
    {
        payload[4 + i] = response[i];
    }

    return payload;
}

struct AuthChallenge
{
    std::uint16_t temporaryAddress { 0 };
    std::array<std::uint8_t, 8> challenge {};
};

// AUTH_KEY_REPLY payload: u16 temporary address, then the 8-byte challenge.
constexpr Result<AuthChallenge> parse_auth_reply(std::span<const std::uint8_t> payload)
{
    if (payload.size() < 10)
    {
        return truncated(static_cast<std::uint16_t>(payload.size()));
    }

    AuthChallenge out;
    out.temporaryAddress = read_u16(payload, 0);
    for (std::size_t i = 0; i < out.challenge.size(); ++i)
    {
        out.challenge[i] = payload[2 + i];
    }

    return out;
}

// CONN_REPLY payload, 14 bytes [CONFIRMED on hardware]:
//   0     protocol version (0x01)
//   1     connection counter
//   2..3  the address assigned to us      <-- what we must send as `src`
//   4     device type, echoed
//   5     0x01
//   6..13 the master's own TEA value (mutual auth; we do not check it)
//
// A rejected authentication comes back all zero, so a zero address is the
// failure signal rather than a separate opcode. Reading the address from
// offset 0 instead of 2 is latent rather than fatal on this radio -- it does
// not validate our `src` -- which is exactly why it survived a capture.
constexpr Result<std::uint16_t> parse_conn_reply(std::span<const std::uint8_t> payload)
{
    if (payload.size() < 4)
    {
        return truncated(static_cast<std::uint16_t>(payload.size()));
    }

    const std::uint16_t address = read_u16(payload, 2);
    if (address == 0)
    {
        return auth_rejected();
    }

    return address;
}

} // namespace mototrbo::xnl

#endif // MOTOTRBO_XNL_H
