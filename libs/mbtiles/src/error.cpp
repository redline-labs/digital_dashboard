// SPDX-License-Identifier: GPL-3.0-or-later
#include "mbtiles/error.h"

namespace mbtiles
{

const char* to_string(Error::Kind kind)
{
    switch (kind)
    {
        case Error::Kind::NotFound:
            return "not found";
        case Error::Kind::NotReadable:
            return "not readable";
        case Error::Kind::NotAnArchive:
            return "not an mbtiles archive";
        case Error::Kind::InvalidArgument:
            return "invalid argument";
        case Error::Kind::Query:
            return "query failed";
    }

    return "unknown";
}

std::string to_string(const Error& error)
{
    std::string out = to_string(error.kind);
    if (!error.message.empty())
    {
        out += ": ";
        out += error.message;
    }
    if (error.code != 0)
    {
        out += " (sqlite ";
        out += std::to_string(error.code);
        out += ")";
    }
    return out;
}

std::unexpected<Error> not_found(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotFound, std::move(message), 0 });
}

std::unexpected<Error> not_readable(std::string message, int code)
{
    return std::unexpected(Error { Error::Kind::NotReadable, std::move(message), code });
}

std::unexpected<Error> not_an_archive(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotAnArchive, std::move(message), 0 });
}

std::unexpected<Error> invalid_argument(std::string message)
{
    return std::unexpected(Error { Error::Kind::InvalidArgument, std::move(message), 0 });
}

std::unexpected<Error> query_error(std::string message, int code)
{
    return std::unexpected(Error { Error::Kind::Query, std::move(message), code });
}

} // namespace mbtiles
