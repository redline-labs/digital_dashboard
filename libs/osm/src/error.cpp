// SPDX-License-Identifier: GPL-3.0-or-later
#include "osm/error.h"

namespace osm
{

const char* to_string(Error::Kind kind)
{
    switch (kind)
    {
        case Error::Kind::Truncated:
            return "truncated";
        case Error::Kind::Malformed:
            return "malformed";
        case Error::Kind::Unsupported:
            return "unsupported";
        case Error::Kind::Decompress:
            return "decompress";
        case Error::Kind::OutOfOrder:
            return "out of order";
        case Error::Kind::Io:
            return "io";
    }

    // After the switch rather than in a default:, so adding a Kind stays a
    // compile error under -Wswitch-enum.
    return "unknown";
}

std::string to_string(const Error& error)
{
    return std::string(to_string(error.kind)) + ": " + error.message + " (at byte " +
           std::to_string(error.offset) + ")";
}

std::unexpected<Error> truncated(std::string message, std::size_t offset)
{
    return std::unexpected(Error { Error::Kind::Truncated, std::move(message), offset });
}

std::unexpected<Error> malformed(std::string message, std::size_t offset)
{
    return std::unexpected(Error { Error::Kind::Malformed, std::move(message), offset });
}

std::unexpected<Error> unsupported(std::string message, std::size_t offset)
{
    return std::unexpected(Error { Error::Kind::Unsupported, std::move(message), offset });
}

std::unexpected<Error> decompress_failed(std::string message, std::size_t offset)
{
    return std::unexpected(Error { Error::Kind::Decompress, std::move(message), offset });
}

std::unexpected<Error> out_of_order(std::string message, std::size_t offset)
{
    return std::unexpected(Error { Error::Kind::OutOfOrder, std::move(message), offset });
}

std::unexpected<Error> io_failed(std::string message)
{
    return std::unexpected(Error { Error::Kind::Io, std::move(message), 0 });
}

std::unexpected<Error> from_wire(const protowire::Error& error, std::size_t blockOffset)
{
    Error::Kind kind = Error::Kind::Malformed;
    switch (error.kind)
    {
        case protowire::Error::Kind::Truncated:
            kind = Error::Kind::Truncated;
            break;
        case protowire::Error::Kind::Malformed:
            kind = Error::Kind::Malformed;
            break;
        case protowire::Error::Kind::Unsupported:
            kind = Error::Kind::Unsupported;
            break;
        case protowire::Error::Kind::Decompress:
            kind = Error::Kind::Decompress;
            break;
    }

    return std::unexpected(Error { kind, error.message, blockOffset + error.offset });
}

} // namespace osm
