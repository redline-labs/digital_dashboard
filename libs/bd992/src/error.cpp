// SPDX-License-Identifier: GPL-3.0-or-later

#include "bd992/error.h"

#include <cstring>

namespace bd992
{

const char* to_string(Error::Kind kind)
{
    switch (kind)
    {
        case Error::Kind::NotFound:        return "not found";
        case Error::Kind::ConnectFailed:   return "connect failed";
        case Error::Kind::NotConnected:    return "not connected";
        case Error::Kind::Io:              return "I/O error";
        case Error::Kind::Timeout:         return "timeout";
        case Error::Kind::Refused:         return "refused by receiver";
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

namespace
{

std::unexpected<Error> make(Error::Kind kind, std::string message, int code = 0)
{
    return std::unexpected(Error { kind, std::move(message), code });
}

} // namespace

std::unexpected<Error> not_found(std::string message)
{
    return make(Error::Kind::NotFound, std::move(message));
}

std::unexpected<Error> connect_failed(std::string message, int code)
{
    return make(Error::Kind::ConnectFailed, std::move(message), code);
}

std::unexpected<Error> not_connected(std::string message)
{
    return make(Error::Kind::NotConnected, std::move(message));
}

std::unexpected<Error> io_error(std::string message, int code)
{
    return make(Error::Kind::Io, std::move(message), code);
}

std::unexpected<Error> timeout(std::string message)
{
    return make(Error::Kind::Timeout, std::move(message));
}

std::unexpected<Error> refused(std::string message)
{
    return make(Error::Kind::Refused, std::move(message));
}

std::unexpected<Error> protocol_error(std::string message)
{
    return make(Error::Kind::Protocol, std::move(message));
}

std::unexpected<Error> invalid_argument(std::string message)
{
    return make(Error::Kind::InvalidArgument, std::move(message));
}

std::unexpected<Error> not_permitted(std::string message)
{
    return make(Error::Kind::NotPermitted, std::move(message));
}

} // namespace bd992
