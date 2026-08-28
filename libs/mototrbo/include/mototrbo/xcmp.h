// SPDX-License-Identifier: GPL-3.0-or-later
//
// XCMP: the command layer that rides inside an XNL data message.
//
// A message is a big-endian u16 opcode followed by a payload. The high bit
// distinguishes the direction: a request has bit 15 clear, its reply is the
// same opcode with bit 15 set, and anything in 0xB4xx is an unsolicited
// broadcast the radio sends on its own. Every reply's first payload byte is a
// result code.
//
// The opcodes below were recovered from the vendor's own client, where each is
// a message class's declared opcode -- so their values are authoritative even
// where this build has never sent one.
//
// WHAT IS DELIBERATELY ABSENT. The radio also implements FactoryReset (0x003F),
// RadioReset (0x000D), EnterTestMode (0x000C), EnterBootMode (0x0200),
// WriteMemory (0x0202), EraseFlash (0x0203), the ISH write/delete/reorg
// opcodes, and Transmit (0x0004). None of them are here, and this library
// keys no transmitter: it builds no PTT command and no RF tuning command. That
// is a scope decision rather than an oversight -- do not sweep the opcode
// space to "complete" this enum.

#ifndef MOTOTRBO_XCMP_H
#define MOTOTRBO_XCMP_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

#include "mototrbo/byte_order.h"
#include "mototrbo/error.h"

namespace mototrbo::xcmp
{

inline constexpr std::uint16_t kReplyBit = 0x8000;

// The 0xB4xx block. Masked rather than enumerated because the radio emits
// broadcasts this build does not model and they must still be recognised as
// broadcasts -- see control::parse_broadcast.
inline constexpr std::uint16_t kBroadcastMask = 0xFF00;
inline constexpr std::uint16_t kBroadcastPrefix = 0xB400;

enum class Opcode : std::uint16_t
{
    RadioStatus      = 0x000E, // one status item, selected by a payload byte
    VersionInfo      = 0x000F, // ASCII, selected by a request-type byte
    // Three alternative reads of things RadioStatus already answers, all seen
    // in a capture of the vendor client talking to this radio. Kept as the
    // record of what the radio implements; nothing here builds one, because
    // one path to the model number is enough and RadioStatus is the one that
    // was exercised.
    ModelNumber      = 0x0010, // [SEEN ON THE WIRE]
    SerialNumber     = 0x0011, // [SEEN ON THE WIRE]
    Uuid             = 0x0012, // [SEEN ON THE WIRE]
    Datecode         = 0x0019, // manufacture date            [CONFIRMED on hardware]
    TanapaNumber     = 0x001F, // kit/part number, ASCII      [CONFIRMED on hardware]
    ZoneChannel      = 0x040D, // channel and zone control    [CONFIRMED on hardware]
};

// Reply result codes -- the first byte of every reply payload. The same table
// the vendor client validates against. The four seen from a real XPR 5550 are
// marked.
enum class ResultCode : std::uint8_t
{
    Success             = 0x00, // [HW]
    Failure             = 0x01,
    IncorrectMode       = 0x02, // [HW] the RF domain outside tuner mode
    OpcodeUnsupported   = 0x03, // [HW]
    InvalidParameter    = 0x04, // [HW] a malformed request, or an out-of-range index
    ReplyTooBig         = 0x05,
    SecurityLocked      = 0x06,
    FunctionUnavailable = 0x07,
    RadioBusy           = 0x11,
    InvalidTarget       = 0x12,
};

constexpr const char* to_string(ResultCode code)
{
    switch (code)
    {
        case ResultCode::Success:             return "success";
        case ResultCode::Failure:             return "failure";
        case ResultCode::IncorrectMode:       return "incorrect mode";
        case ResultCode::OpcodeUnsupported:   return "opcode not supported";
        case ResultCode::InvalidParameter:    return "invalid parameter";
        case ResultCode::ReplyTooBig:         return "reply too big";
        case ResultCode::SecurityLocked:      return "security locked";
        case ResultCode::FunctionUnavailable: return "function not available";
        case ResultCode::RadioBusy:           return "radio busy";
        case ResultCode::InvalidTarget:       return "invalid target address";
    }

    return "unknown result code";
}

// ---------------------------------------------------------------------------
// A built request.
//
// Every command this library sends is an opcode and at most a handful of
// bytes, so a Command is a small fixed array rather than a vector. That is
// what makes the builders in control.h constexpr, which in turn is what lets
// their encodings be asserted at build time against bytes a real radio
// accepted -- and it keeps the command path free of allocation.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxCommandSize = 12;

class Command
{
  public:
    constexpr Command() = default;

    constexpr Command(Opcode opcode, std::initializer_list<std::uint8_t> payload = {})
    {
        write_u16(mBytes, 0, static_cast<std::uint16_t>(opcode));
        mSize = 2;
        for (const std::uint8_t byte : payload)
        {
            mBytes[mSize++] = byte;
        }
    }

    constexpr std::span<const std::uint8_t> bytes() const
    {
        return std::span<const std::uint8_t>(mBytes.data(), mSize);
    }

    constexpr std::uint16_t opcode() const { return read_u16(mBytes, 0); }

    // The opcode the matching reply will carry.
    constexpr std::uint16_t replyOpcode() const { return opcode() | kReplyBit; }

  private:
    std::array<std::uint8_t, kMaxCommandSize> mBytes {};
    // Bounded by kMaxCommandSize, so the initializer_list above cannot
    // overrun: a longer one is a compile error in a constexpr context and an
    // out-of-range subscript at run time under the sanitizers.
    std::uint8_t mSize { 0 };
};

// A parsed message. `payload` is a VIEW into the caller's buffer.
struct Message
{
    std::uint16_t opcode { 0 };
    std::span<const std::uint8_t> payload;

    // Only the broadcast test is offered, deliberately. "Is this the reply I
    // am waiting for" is not a property of a message on its own -- it is the
    // message's opcode AND the echoed transaction id, and a helper that
    // answered half of it would invite the correlation bug back.
    constexpr bool isBroadcast() const { return (opcode & kBroadcastMask) == kBroadcastPrefix; }
};

constexpr Result<Message> parse_message(std::span<const std::uint8_t> raw)
{
    if (raw.size() < 2)
    {
        return truncated(static_cast<std::uint16_t>(raw.size()));
    }

    return Message { read_u16(raw, 0), raw.subspan(2) };
}

// The reply body: everything after the result code, or the result code as an
// error. Returning the refusal as an error rather than as data is deliberate --
// a caller that forgot to check would otherwise read the result byte as the
// first byte of the value.
constexpr Result<std::span<const std::uint8_t>> reply_body(const Message& message)
{
    if (message.payload.empty())
    {
        return truncated(0);
    }
    if (message.payload[0] != static_cast<std::uint8_t>(ResultCode::Success))
    {
        return device_nak(message.payload[0]);
    }

    return message.payload.subspan(1);
}

} // namespace mototrbo::xcmp

#endif // MOTOTRBO_XCMP_H
