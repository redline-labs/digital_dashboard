// SPDX-License-Identifier: GPL-3.0-or-later

#include "mti610/error.h"

#include <cstring>
#include <utility>

namespace mti610
{

const char* to_string(Error::Kind kind)
{
    switch (kind)
    {
        case Error::Kind::NotFound:        return "not found";
        case Error::Kind::OpenFailed:      return "open failed";
        case Error::Kind::NotConnected:    return "not connected";
        case Error::Kind::Io:              return "I/O error";
        case Error::Kind::Timeout:         return "timeout";
        case Error::Kind::Refused:         return "refused by device";
        case Error::Kind::Protocol:        return "protocol error";
        case Error::Kind::InvalidArgument: return "invalid argument";
        case Error::Kind::NotPermitted:    return "not permitted";
    }

    return "unknown";
}

std::string to_string(const Error& error)
{
    std::string out = to_string(error.kind);
    if (!error.message.empty())
    {
        out += ": " + error.message;
    }
    if (error.code != 0)
    {
        out += " (";
        out += std::strerror(error.code);
        out += ")";
    }
    return out;
}

std::unexpected<Error> not_found(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotFound, std::move(message), 0 });
}

std::unexpected<Error> open_failed(std::string message, int code)
{
    return std::unexpected(Error { Error::Kind::OpenFailed, std::move(message), code });
}

std::unexpected<Error> not_connected(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotConnected, std::move(message), 0 });
}

std::unexpected<Error> io_error(std::string message, int code)
{
    return std::unexpected(Error { Error::Kind::Io, std::move(message), code });
}

std::unexpected<Error> timeout(std::string message)
{
    return std::unexpected(Error { Error::Kind::Timeout, std::move(message), 0 });
}

std::unexpected<Error> refused(std::string message)
{
    return std::unexpected(Error { Error::Kind::Refused, std::move(message), 0 });
}

std::unexpected<Error> protocol_error(std::string message)
{
    return std::unexpected(Error { Error::Kind::Protocol, std::move(message), 0 });
}

std::unexpected<Error> invalid_argument(std::string message)
{
    return std::unexpected(Error { Error::Kind::InvalidArgument, std::move(message), 0 });
}

std::unexpected<Error> not_permitted(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotPermitted, std::move(message), 0 });
}

} // namespace mti610
