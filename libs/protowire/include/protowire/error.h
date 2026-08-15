// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong reading protobuf off the wire.
//
// Same shape as can::Error and mbtiles::Error. Deliberately NOT gsof::Error's
// literal-only form: a malformed message is worth saying *where* it went wrong,
// because the one that fails is one of tens of thousands -- a tile in a
// viewport, a block in a continental PBF -- and the offset is the only way to
// find it again.
//
// `Decompress` is the odd one out: zlib is not a protobuf concern. It is here
// because both formats this serves store their payloads compressed and both
// inflate before parsing, and because splitting it out at the same time as the
// move out of libs/mvt would have weakened the "this changed nothing" argument
// that made the move safe.
#ifndef PROTOWIRE_ERROR_H
#define PROTOWIRE_ERROR_H

#include <cstddef>
#include <expected>
#include <string>

namespace protowire
{

struct Error
{
    enum class Kind
    {
        // The buffer ended in the middle of something -- a varint, a
        // length-delimited field, a geometry command's parameters.
        Truncated,
        // A field is present but cannot mean what it says: a length that
        // overruns the buffer, a tag index past the end of the key table, a
        // geometry command id that is not MoveTo/LineTo/ClosePath.
        Malformed,
        // Structurally fine, but not something this decoder handles -- a
        // protobuf wire type MVT does not use.
        Unsupported,
        // zlib said no.
        Decompress,
    };

    Kind kind { Kind::Malformed };
    std::string message;
    // Byte offset into the tile where it went wrong. Zero when not applicable.
    std::size_t offset { 0 };
};

const char* to_string(Error::Kind kind);
std::string to_string(const Error& error);

template <typename T>
using Result = std::expected<T, Error>;

std::unexpected<Error> truncated(std::string message, std::size_t offset = 0);
std::unexpected<Error> malformed(std::string message, std::size_t offset = 0);
std::unexpected<Error> unsupported(std::string message, std::size_t offset = 0);
std::unexpected<Error> decompress_failed(std::string message);

} // namespace protowire

#endif // PROTOWIRE_ERROR_H
