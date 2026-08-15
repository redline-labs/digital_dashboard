// SPDX-License-Identifier: GPL-3.0-or-later
#include "protowire/error.h"

namespace protowire
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
            return "decompression failed";
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
    if (error.offset != 0)
    {
        out += " (at byte ";
        out += std::to_string(error.offset);
        out += ")";
    }
    return out;
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

std::unexpected<Error> decompress_failed(std::string message)
{
    return std::unexpected(Error { Error::Kind::Decompress, std::move(message), 0 });
}

} // namespace protowire
