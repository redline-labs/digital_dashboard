// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong when building a command for an MSEL Master Relay.
//
// Only two kinds, because only two responses exist. `InvalidArgument` means the
// caller asked for something the device cannot be told -- an address wider than
// 11 bits, a shutdown delay longer than the field holds, an output drive code
// the manual does not assign. Retrying with the same value will fail the same
// way, so it belongs in front of a user. `Unsupported` means the request is
// well formed but this device will not do it: the remote kill has not been
// enabled on the node, or the relay is too old to have the feature.
//
// There is deliberately no `Io` kind. This library never transmits anything --
// it builds frames and decodes them, and whoever puts them on a bus owns the
// failures that come with doing so. See can::Error for the errors that live on
// that side of the line.
#ifndef MSEL_ERROR_H
#define MSEL_ERROR_H

#include <expected>
#include <string>

namespace msel
{

struct Error
{
    enum class Kind
    {
        // The value cannot be encoded, and no retry will change that.
        InvalidArgument,
        // Well formed, but not available: gated off, or absent on this
        // firmware.
        Unsupported,
    };

    Kind kind { Kind::InvalidArgument };
    std::string message;
};

const char* to_string(Error::Kind kind);
std::string to_string(const Error& error);

template <typename T>
using Result = std::expected<T, Error>;

std::unexpected<Error> invalid_argument(std::string message);
std::unexpected<Error> unsupported(std::string message);

} // namespace msel

#endif // MSEL_ERROR_H
