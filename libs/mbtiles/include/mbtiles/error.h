// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong reading an .mbtiles archive.
//
// Same shape as can::Error and bd992::Error rather than gsof::Error: nothing
// here is constexpr, the failures all come from a file on disk, and the useful
// part of "this archive is not usable" is which file and what SQLite said about
// it. A literal-only error would throw that away.
//
// Note what is NOT an error: a tile that is not in the archive. Coverage is a
// bounding box at best and sparse inside it, so Archive::tile() returns an
// empty optional for that and reserves Error for archives that are broken.
#ifndef MBTILES_ERROR_H
#define MBTILES_ERROR_H

#include <expected>
#include <string>

namespace mbtiles
{

struct Error
{
    enum class Kind
    {
        // No file at that path.
        NotFound,
        // The file is there but cannot be opened -- permissions, or a
        // directory where a file was expected.
        NotReadable,
        // Opened, but it is not an SQLite database, or not one shaped like an
        // .mbtiles: no `tiles`, no `metadata`.
        NotAnArchive,
        // The request itself is wrong: a zoom past 31 where 2^z stops fitting,
        // an x or y outside 2^z.
        InvalidArgument,
        // SQLite failed on a query that should have worked. A corrupt file, or
        // one that changed underneath us.
        Query,
    };

    Kind kind { Kind::Query };
    std::string message;
    // The SQLite result code, when there is one. Zero otherwise.
    int code { 0 };
};

const char* to_string(Error::Kind kind);
std::string to_string(const Error& error);

template <typename T>
using Result = std::expected<T, Error>;

std::unexpected<Error> not_found(std::string message);
std::unexpected<Error> not_readable(std::string message, int code = 0);
std::unexpected<Error> not_an_archive(std::string message);
std::unexpected<Error> invalid_argument(std::string message);
std::unexpected<Error> query_error(std::string message, int code = 0);

} // namespace mbtiles

#endif // MBTILES_ERROR_H
