// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong when decoding a MOTOTRBO frame, in a form that survives
// constant evaluation.
//
// Same argument as gsof::Error, and the same shape: an enum plus two small
// integers, no allocation, so that `static_assert(parse(bad).error().kind ==
// ...)` compiles. Anything that owns a socket reports xpr::Error instead,
// which carries a message and an errno and is never used in a constant
// expression.
//
// `detail` is the one field worth explaining. A radio that refuses a command
// answers with a result code rather than silence, and WHICH code it is decides
// what the caller should do: 0x02 incorrect_mode means the radio is not in a
// state where this works, 0x03 opcode_unsupported means this radio will never
// do it, 0x04 invalid_parameter means the request was malformed. Folding all
// three into "NAK" throws away the only thing that distinguishes "try later"
// from "stop asking".

#ifndef MOTOTRBO_ERROR_H
#define MOTOTRBO_ERROR_H

#include <cstdint>
#include <expected>

namespace mototrbo
{

enum class ErrorKind : std::uint8_t
{
    // The buffer ended before the field being read did. On a stream this means
    // "come back with more bytes" rather than "this is broken", so callers
    // that are reassembling must distinguish it from the rest.
    Truncated,
    // A length field disagrees with the frame that contains it.
    LengthMismatch,
    // A reply arrived whose opcode is not the one the request asked for, or a
    // broadcast was handed to a parser that only takes replies.
    UnexpectedOpcode,
    // The radio understood the request and declined it. `detail` is the XCMP
    // result code -- see xcmp::ResultCode.
    DeviceNak,
    // The master rejected the authentication response, or the handshake did
    // not produce an address.
    AuthRejected,
    // The request itself is wrong: a payload too long to frame, a status item
    // this build does not know.
    InvalidArgument,
};

struct Error
{
    ErrorKind kind { ErrorKind::Truncated };

    // The XCMP result code for DeviceNak; the low byte of the offending opcode
    // for UnexpectedOpcode. Zero otherwise.
    std::uint8_t detail { 0 };

    // Byte offset into the buffer the failing parser was handed.
    std::uint16_t offset { 0 };
};

constexpr bool operator==(const Error& a, const Error& b)
{
    return a.kind == b.kind && a.detail == b.detail && a.offset == b.offset;
}

template <typename T>
using Result = std::expected<T, Error>;

constexpr std::unexpected<Error> truncated(std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::Truncated, 0, offset });
}

constexpr std::unexpected<Error> length_mismatch(std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::LengthMismatch, 0, offset });
}

constexpr std::unexpected<Error> unexpected_opcode(std::uint16_t opcode)
{
    return std::unexpected(Error { ErrorKind::UnexpectedOpcode, static_cast<std::uint8_t>(opcode & 0xFFu), 0 });
}

constexpr std::unexpected<Error> device_nak(std::uint8_t resultCode)
{
    return std::unexpected(Error { ErrorKind::DeviceNak, resultCode, 0 });
}

constexpr std::unexpected<Error> auth_rejected()
{
    return std::unexpected(Error { ErrorKind::AuthRejected, 0, 0 });
}

constexpr std::unexpected<Error> invalid_argument()
{
    return std::unexpected(Error { ErrorKind::InvalidArgument, 0, 0 });
}

constexpr const char* to_string(ErrorKind kind)
{
    switch (kind)
    {
        case ErrorKind::Truncated:       return "truncated";
        case ErrorKind::LengthMismatch:  return "length mismatch";
        case ErrorKind::UnexpectedOpcode:return "unexpected opcode";
        case ErrorKind::DeviceNak:       return "radio refused the command";
        case ErrorKind::AuthRejected:    return "authentication rejected";
        case ErrorKind::InvalidArgument: return "invalid argument";
    }

    return "unknown";
}

} // namespace mototrbo

#endif // MOTOTRBO_ERROR_H
