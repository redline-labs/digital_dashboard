// SPDX-License-Identifier: GPL-3.0-or-later

#include "xpr/error.h"

#include "mototrbo/xcmp.h"

namespace xpr
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
        case Error::Kind::Refused:         return "refused by the radio";
        case Error::Kind::Protocol:        return "protocol error";
        case Error::Kind::AuthFailed:      return "authentication failed";
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

    if (error.kind == Error::Kind::Refused)
    {
        out += " (";
        out += mototrbo::xcmp::to_string(static_cast<mototrbo::xcmp::ResultCode>(error.code));
        out += ")";
    }

    return out;
}

std::unexpected<Error> not_found(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotFound, std::move(message), 0 });
}

std::unexpected<Error> connect_failed(std::string message, int code)
{
    return std::unexpected(Error { Error::Kind::ConnectFailed, std::move(message), code });
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

std::unexpected<Error> refused(std::string message, int resultCode)
{
    return std::unexpected(Error { Error::Kind::Refused, std::move(message), resultCode });
}

std::unexpected<Error> protocol_error(std::string message)
{
    return std::unexpected(Error { Error::Kind::Protocol, std::move(message), 0 });
}

std::unexpected<Error> auth_failed(std::string message)
{
    return std::unexpected(Error { Error::Kind::AuthFailed, std::move(message), 0 });
}

std::unexpected<Error> invalid_argument(std::string message)
{
    return std::unexpected(Error { Error::Kind::InvalidArgument, std::move(message), 0 });
}

std::unexpected<Error> not_permitted(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotPermitted, std::move(message), 0 });
}

std::unexpected<Error> from_protocol(const mototrbo::Error& error, std::string context)
{
    const std::string detail = context + ": " + mototrbo::to_string(error.kind);

    switch (error.kind)
    {
        case mototrbo::ErrorKind::DeviceNak:
            return refused(context, error.detail);
        case mototrbo::ErrorKind::AuthRejected:
            return auth_failed(detail);
        case mototrbo::ErrorKind::InvalidArgument:
            return invalid_argument(detail);
        case mototrbo::ErrorKind::Truncated:
        case mototrbo::ErrorKind::LengthMismatch:
        case mototrbo::ErrorKind::UnexpectedOpcode:
            return protocol_error(detail);
    }

    return protocol_error(detail);
}

} // namespace xpr
