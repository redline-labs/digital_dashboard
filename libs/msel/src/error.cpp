// SPDX-License-Identifier: GPL-3.0-or-later

#include "msel/error.h"

#include <spdlog/fmt/fmt.h>

namespace msel
{

const char* to_string(Error::Kind kind)
{
    switch (kind)
    {
    case Error::Kind::InvalidArgument: return "invalid argument";
    case Error::Kind::Unsupported: return "unsupported";
    }
    return "error";
}

std::string to_string(const Error& error)
{
    return fmt::format("{}: {}", to_string(error.kind), error.message);
}

std::unexpected<Error> invalid_argument(std::string message)
{
    return std::unexpected(Error { Error::Kind::InvalidArgument, std::move(message) });
}

std::unexpected<Error> unsupported(std::string message)
{
    return std::unexpected(Error { Error::Kind::Unsupported, std::move(message) });
}

} // namespace msel
