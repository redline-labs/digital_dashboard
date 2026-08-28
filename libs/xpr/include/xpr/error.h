// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong when talking to a radio over a network, in a form a caller
// can act on.
//
// Separate from mototrbo::Error, which describes bytes that did not decode and
// is deliberately a literal type so the parsers can be constexpr. This one
// describes a connection: it carries a message and an integer, and is never
// used in a constant expression.
//
// The kinds are distinguished because the responses differ. `NotConnected`
// clears on its own once the reconnect succeeds. `Timeout` means the radio is
// there but did not answer -- normal while it is busy. `Refused` means it
// answered with an XCMP result code and sending the same bytes again will get
// the same answer, so `code` carries that result code rather than an errno --
// the difference between "not in this mode" and "this radio will never do
// that" is the whole reason to look.

#ifndef XPR_ERROR_H
#define XPR_ERROR_H

#include <expected>
#include <string>

#include "mototrbo/error.h"

namespace xpr
{

struct Error
{
    enum class Kind
    {
        // The host name did not resolve, or the address is malformed.
        NotFound,
        // connect() failed -- nothing is listening, or the network is down.
        ConnectFailed,
        // There is no live session. Distinct from ConnectFailed: this is the
        // state between a dropped connection and the next attempt.
        NotConnected,
        // A send or receive failed, or the radio closed mid-exchange.
        Io,
        // No answer inside the deadline.
        Timeout,
        // The radio answered with a non-zero XCMP result code; `code` is it.
        Refused,
        // The radio answered with something the protocol does not allow, or
        // the XNL handshake did not complete.
        Protocol,
        // Authentication was rejected. On this link that means the TEA
        // response was wrong, which is a bug rather than a configuration
        // problem -- the key is fixed.
        AuthFailed,
        // The request itself is wrong: an unknown status item, a channel that
        // does not exist.
        InvalidArgument,
        // Refused locally rather than by the radio: a channel change while
        // the node is configured read-only, or a zone change, which this
        // radio does not support over XNL at all.
        NotPermitted,
    };

    Kind kind { Kind::Io };
    std::string message;
    // errno for the I/O kinds, the XCMP result code for Refused. Zero
    // otherwise.
    int code { 0 };
};

const char* to_string(Error::Kind kind);
std::string to_string(const Error& error);

template <typename T>
using Result = std::expected<T, Error>;

std::unexpected<Error> not_found(std::string message);
std::unexpected<Error> connect_failed(std::string message, int code = 0);
std::unexpected<Error> not_connected(std::string message);
std::unexpected<Error> io_error(std::string message, int code = 0);
std::unexpected<Error> timeout(std::string message);
std::unexpected<Error> refused(std::string message, int resultCode);
std::unexpected<Error> protocol_error(std::string message);
std::unexpected<Error> auth_failed(std::string message);
std::unexpected<Error> invalid_argument(std::string message);
std::unexpected<Error> not_permitted(std::string message);

// Lift a decode failure into a connection-level error, keeping the radio's own
// result code where there is one. A refusal must not arrive at the caller as a
// generic "protocol error": those are the two ends of the diagnostic and
// flattening them together is how "the radio said no" becomes "something is
// broken".
std::unexpected<Error> from_protocol(const mototrbo::Error& error, std::string context);

} // namespace xpr

#endif // XPR_ERROR_H
