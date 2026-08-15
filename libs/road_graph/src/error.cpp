// SPDX-License-Identifier: GPL-3.0-or-later
#include "road_graph/error.h"

namespace road_graph
{

const char* to_string(Error::Kind kind)
{
    switch (kind)
    {
        case Error::Kind::NotFound:
            return "not found";
        case Error::Kind::NotReadable:
            return "not readable";
        case Error::Kind::NotAGraph:
            return "not a graph";
        case Error::Kind::VersionMismatch:
            return "version mismatch";
        case Error::Kind::Malformed:
            return "malformed";
        case Error::Kind::InvalidArgument:
            return "invalid argument";
    }

    // After the switch rather than in a default:, so adding a Kind stays a
    // compile error under -Wswitch-enum.
    return "unknown";
}

std::string to_string(const Error& error)
{
    return std::string(to_string(error.kind)) + ": " + error.message;
}

std::unexpected<Error> not_found(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotFound, std::move(message) });
}

std::unexpected<Error> not_readable(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotReadable, std::move(message) });
}

std::unexpected<Error> not_a_graph(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotAGraph, std::move(message) });
}

std::unexpected<Error> version_mismatch(std::string message)
{
    return std::unexpected(Error { Error::Kind::VersionMismatch, std::move(message) });
}

std::unexpected<Error> malformed(std::string message)
{
    return std::unexpected(Error { Error::Kind::Malformed, std::move(message) });
}

std::unexpected<Error> invalid_argument(std::string message)
{
    return std::unexpected(Error { Error::Kind::InvalidArgument, std::move(message) });
}

} // namespace road_graph
