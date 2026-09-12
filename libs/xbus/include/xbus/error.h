// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong when decoding an XBus message, in a form that survives
// constant evaluation.
//
// The constraint is the same one gsof/error.h works under: every parser in
// this library is constexpr so that a byte vector plus a static_assert turns a
// wrong field offset into a build failure, and a member that allocates makes
// an error value non-literal -- it cannot be stored in a constexpr variable,
// and `static_assert(parse(bad).error().kind == ...)` stops compiling. So the
// error is an enum plus two integers, and the prose lives in to_string().
//
// The context fields exist because "Truncated" alone is useless in a log: a
// stream that half-works reports it once per second and you need to know which
// data identifier and how far in. `offset` is measured from the start of
// whatever buffer the parser was handed.

#ifndef XBUS_ERROR_H
#define XBUS_ERROR_H

#include <cstdint>
#include <expected>

namespace xbus
{

enum class ErrorKind : std::uint8_t
{
    // The buffer ended before the field being read did. For the framer this is
    // routine -- it means "come back with more bytes" -- so callers there must
    // distinguish it from the rest rather than logging it.
    Truncated,
    // Checksum mismatch. The bytes arrived, they are just not what was sent.
    BadChecksum,
    // The front byte is not the 0xFA preamble.
    BadFraming,
    // A data item's LEN disagrees with the fixed size of the identifier it
    // carries. Unlike a GSOF record, an MTData2 item of the wrong length is
    // never legitimate: the payload sizes are fixed by the format nibble.
    LengthMismatch,
    // A data identifier that is not in XBUS_DATA_TABLE. Not fatal: an MTData2
    // item is self-delimiting, so an unmodelled one is skipped, counted, and
    // passed through as raw bytes.
    UnknownDataId,
    // A precision the identifier's format nibble names but this library does
    // not decode for that group -- in practice a coordinate-system bit set on
    // something that has no coordinate system.
    UnsupportedFormat,
    // A declared length beyond what the protocol permits: more than 2048 data
    // bytes in an extended-length message, or an extended length where the
    // standard form was required.
    TooLong,
    // The device answered, but with Error (MID 0x42) rather than the expected
    // acknowledgement. `detail` carries the MTi's own error code -- see
    // to_string(DeviceError).
    DeviceReportedError,
};

struct Error
{
    ErrorKind kind { ErrorKind::Truncated };

    // The message id or data identifier being parsed when this happened, or 0
    // when the failure was in the outer framing and no message was involved.
    // Truncated to 8 bits: a data identifier's low byte is its type and format
    // nibbles, which is the half that identifies which parser failed.
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

// Shorthands, so a call site reads as the thing that went wrong rather than as
// three lines of aggregate initialisation.
constexpr std::unexpected<Error> truncated(std::uint16_t offset = 0, std::uint8_t detail = 0)
{
    return std::unexpected(Error { ErrorKind::Truncated, detail, offset });
}

constexpr std::unexpected<Error> bad_checksum(std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::BadChecksum, 0, offset });
}

constexpr std::unexpected<Error> bad_framing(std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::BadFraming, 0, offset });
}

constexpr std::unexpected<Error> length_mismatch(std::uint8_t detail = 0, std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::LengthMismatch, detail, offset });
}

constexpr std::unexpected<Error> unknown_data_id(std::uint8_t detail, std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::UnknownDataId, detail, offset });
}

constexpr std::unexpected<Error> unsupported_format(std::uint8_t detail, std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::UnsupportedFormat, detail, offset });
}

constexpr std::unexpected<Error> too_long(std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::TooLong, 0, offset });
}

constexpr std::unexpected<Error> device_reported_error(std::uint8_t code)
{
    return std::unexpected(Error { ErrorKind::DeviceReportedError, code, 0 });
}

constexpr const char* to_string(ErrorKind kind)
{
    switch (kind)
    {
        case ErrorKind::Truncated:           return "truncated";
        case ErrorKind::BadChecksum:         return "bad checksum";
        case ErrorKind::BadFraming:          return "bad framing";
        case ErrorKind::LengthMismatch:      return "length mismatch";
        case ErrorKind::UnknownDataId:       return "unknown data identifier";
        case ErrorKind::UnsupportedFormat:   return "unsupported data format";
        case ErrorKind::TooLong:             return "too long";
        case ErrorKind::DeviceReportedError: return "device reported an error";
    }

    return "unknown";
}

// The error codes an MTi sends in the payload of MID 0x42.
//
// LLCP table 5. Not exhaustive -- the LLCP points at XsResultValue in the SDK
// doxygen for the full list -- so an unrecognised code is reported as its
// number rather than folded into one of these.
enum class DeviceError : std::uint8_t
{
    PeriodOutOfRange = 0x03,
    MessageInvalid = 0x04,
    // Almost always self-inflicted: an output configuration asking for more
    // than the serial link can carry at the configured baud rate.
    TimerOverflow = 0x1E,
    BaudRateOutOfRange = 0x20,
    ParameterInvalid = 0x21,
    // Carries five further bytes of device detail.
    DeviceError = 0x28,
};

constexpr const char* to_string(DeviceError code)
{
    switch (code)
    {
        case DeviceError::PeriodOutOfRange:   return "period out of range";
        case DeviceError::MessageInvalid:     return "message invalid";
        case DeviceError::TimerOverflow:      return "timer overflow (output rate too high for the link)";
        case DeviceError::BaudRateOutOfRange: return "baud rate out of range";
        case DeviceError::ParameterInvalid:   return "parameter invalid or out of range";
        case DeviceError::DeviceError:        return "device error (try updating the firmware)";
    }

    // Not a default: -- the switch must keep failing to compile when a code is
    // added. An MTi may legitimately send a code from the wider XsResultValue
    // list that this build has never heard of, and that is what lands here.
    return "unrecognised error code";
}

} // namespace xbus

#endif // XBUS_ERROR_H
