// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong reading an OpenStreetMap PBF.
//
// Same shape as protowire::Error and mbtiles::Error. The offset matters more
// here than anywhere else in the tree: a continental extract is tens of
// thousands of independently compressed blocks, and "block 41207 at byte
// 9138472" is the only way to find the one that failed again.
#ifndef OSM_ERROR_H
#define OSM_ERROR_H

#include <cstddef>
#include <expected>
#include <string>

#include "protowire/error.h"

namespace osm
{

struct Error
{
    enum class Kind
    {
        // The file ended in the middle of something -- a blob header, a
        // length-delimited field, a packed array.
        Truncated,
        // Structurally present but cannot mean what it says: a blob larger than
        // the spec allows, a coordinate index past the end of the string table,
        // a member type that is not node/way/relation.
        Malformed,
        // Understood and refused. A compression codec we do not implement, a
        // required_features string this build does not know.
        Unsupported,
        // zlib said no.
        Decompress,
        // The file is not in (type, id) order. Not a format violation -- the
        // ordering is advisory -- but the two-pass node store cannot be built
        // from a file that lacks it, so it is refused rather than silently
        // producing a graph with holes. See node_store.h.
        OutOfOrder,
        // Could not read the file at all.
        Io,
    };

    Kind kind { Kind::Malformed };
    std::string message;
    // Byte offset into the file, or into the block, where it went wrong.
    std::size_t offset { 0 };
};

const char* to_string(Error::Kind kind);
std::string to_string(const Error& error);

template <typename T>
using Result = std::expected<T, Error>;

std::unexpected<Error> truncated(std::string message, std::size_t offset = 0);
std::unexpected<Error> malformed(std::string message, std::size_t offset = 0);
std::unexpected<Error> unsupported(std::string message, std::size_t offset = 0);
std::unexpected<Error> decompress_failed(std::string message, std::size_t offset = 0);
std::unexpected<Error> out_of_order(std::string message, std::size_t offset = 0);
std::unexpected<Error> io_failed(std::string message);

// A wire-level failure, carried up with its offset rebased onto the file.
//
// protowire reports offsets within the buffer it was handed, which for a PBF is
// one inflated block; adding the block's own offset is what makes the number
// point at something a person can find with `xxd`.
std::unexpected<Error> from_wire(const protowire::Error& error, std::size_t blockOffset = 0);

} // namespace osm

#endif // OSM_ERROR_H
