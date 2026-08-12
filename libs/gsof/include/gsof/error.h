// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong when decoding a Trimble packet, in a form that survives
// constant evaluation.
//
// This deliberately does NOT look like can::Error, which carries a
// std::string. Every parser in this library is constexpr so that a golden byte
// vector plus a static_assert turns a wrong field offset into a build failure,
// and a member that allocates makes an error value non-literal: it cannot be
// stored in a constexpr variable, and `static_assert(parse(bad).error().kind
// == ...)` stops compiling. So the error is an enum plus two integers, and the
// prose lives in to_string().
//
// The context fields exist because "Truncated" on its own is useless in a log:
// a stream that half-works reports it once per second and you need to know
// which record type and how far in. `offset` is measured from the start of
// whatever buffer the parser was handed.
#ifndef GSOF_ERROR_H
#define GSOF_ERROR_H

#include <cstdint>
#include <expected>

namespace gsof
{

enum class ErrorKind : std::uint8_t
{
    // The buffer ended before the field being read did. For the framer this is
    // routine -- it means "come back with more bytes" -- so callers there must
    // distinguish it from the rest rather than logging it.
    Truncated,
    // Checksum mismatch. The bytes arrived, they are just not what was sent.
    BadChecksum,
    // STX or ETX is not where the length field says it should be.
    BadFraming,
    // A length field disagrees with the fixed size of the thing it describes,
    // or with the count of elements that follow it.
    LengthMismatch,
    // A record type that is not in GSOF_RECORD_TABLE. Not fatal: the record
    // framing is self-describing, so an unmodelled record can be skipped or
    // passed through as raw bytes.
    UnknownRecord,
    // A page arrived that does not continue the transmission being assembled --
    // out of order, duplicated, or from a different transmission number.
    PageOutOfOrder,
    // More pages, or more bytes, than the assembler is willing to hold. A
    // bound exists so a receiver that never sends a final page cannot grow the
    // buffer without limit.
    TooLong,
};

struct Error
{
    ErrorKind kind { ErrorKind::Truncated };

    // The GSOF record type being parsed when this happened, or 0 when the
    // failure was in the outer framing and no record was involved.
    std::uint8_t recordType { 0 };

    // Byte offset into the buffer the failing parser was handed.
    std::uint16_t offset { 0 };
};

constexpr bool operator==(const Error& a, const Error& b)
{
    return a.kind == b.kind && a.recordType == b.recordType && a.offset == b.offset;
}

template <typename T>
using Result = std::expected<T, Error>;

// Shorthands, so a call site reads as the thing that went wrong rather than as
// three lines of aggregate initialisation.
constexpr std::unexpected<Error> truncated(std::uint16_t offset = 0, std::uint8_t recordType = 0)
{
    return std::unexpected(Error { ErrorKind::Truncated, recordType, offset });
}

constexpr std::unexpected<Error> bad_checksum(std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::BadChecksum, 0, offset });
}

constexpr std::unexpected<Error> bad_framing(std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::BadFraming, 0, offset });
}

constexpr std::unexpected<Error> length_mismatch(std::uint8_t recordType = 0, std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::LengthMismatch, recordType, offset });
}

constexpr std::unexpected<Error> unknown_record(std::uint8_t recordType, std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::UnknownRecord, recordType, offset });
}

constexpr std::unexpected<Error> page_out_of_order(std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::PageOutOfOrder, 0, offset });
}

constexpr std::unexpected<Error> too_long(std::uint16_t offset = 0)
{
    return std::unexpected(Error { ErrorKind::TooLong, 0, offset });
}

constexpr const char* to_string(ErrorKind kind)
{
    switch (kind)
    {
        case ErrorKind::Truncated:      return "truncated";
        case ErrorKind::BadChecksum:    return "bad checksum";
        case ErrorKind::BadFraming:     return "bad framing";
        case ErrorKind::LengthMismatch: return "length mismatch";
        case ErrorKind::UnknownRecord:  return "unknown record type";
        case ErrorKind::PageOutOfOrder: return "page out of order";
        case ErrorKind::TooLong:        return "too long";
    }

    return "unknown";
}

} // namespace gsof

#endif // GSOF_ERROR_H
