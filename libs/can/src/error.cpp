// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/error.h"

#include <spdlog/fmt/fmt.h>

namespace can
{

const char* to_string(Error::Kind kind)
{
    switch (kind)
    {
    case Error::Kind::NotFound: return "not found";
    case Error::Kind::Busy: return "busy";
    case Error::Kind::PermissionDenied: return "permission denied";
    case Error::Kind::Unsupported: return "unsupported";
    case Error::Kind::InvalidArgument: return "invalid argument";
    case Error::Kind::InvalidState: return "invalid state";
    case Error::Kind::Io: return "I/O error";
    case Error::Kind::Protocol: return "protocol error";
    }
    return "error";
}

std::string to_string(const Error& error)
{
    if (error.code != 0)
    {
        return fmt::format("{}: {} ({})", to_string(error.kind), error.message, error.code);
    }
    return fmt::format("{}: {}", to_string(error.kind), error.message);
}

std::unexpected<Error> not_found(std::string message)
{
    return std::unexpected(Error { Error::Kind::NotFound, std::move(message), 0 });
}

std::unexpected<Error> busy(std::string message)
{
    return std::unexpected(Error { Error::Kind::Busy, std::move(message), 0 });
}

std::unexpected<Error> permission_denied(std::string message)
{
    return std::unexpected(Error { Error::Kind::PermissionDenied, std::move(message), 0 });
}

std::unexpected<Error> unsupported(std::string message)
{
    return std::unexpected(Error { Error::Kind::Unsupported, std::move(message), 0 });
}

std::unexpected<Error> invalid_argument(std::string message)
{
    return std::unexpected(Error { Error::Kind::InvalidArgument, std::move(message), 0 });
}

std::unexpected<Error> invalid_state(std::string message)
{
    return std::unexpected(Error { Error::Kind::InvalidState, std::move(message), 0 });
}

std::unexpected<Error> io_error(std::string message, int code)
{
    return std::unexpected(Error { Error::Kind::Io, std::move(message), code });
}

std::unexpected<Error> protocol_error(std::string message)
{
    return std::unexpected(Error { Error::Kind::Protocol, std::move(message), 0 });
}

} // namespace can
