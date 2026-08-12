// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong when talking to a receiver over a network, in a form a
// caller can act on.
//
// Separate from gsof::Error, which describes bytes that did not decode and is
// deliberately a literal type so parsers can be constexpr. This one describes
// a connection, carries a message and an errno, and is never used in a
// constant expression. The kinds are distinguished because the responses
// differ: `NotConnected` means retry once the reader thread reconnects,
// `Timeout` means the receiver is there but did not answer, `Refused` means it
// answered with a NAK and retrying the same command will get the same answer.
#ifndef BD992_ERROR_H
#define BD992_ERROR_H

#include <expected>
#include <string>

namespace bd992
{

struct Error
{
    enum class Kind
    {
        // The host name did not resolve, or the address is malformed.
        NotFound,
        // connect() failed -- nothing is listening, or the network is down.
        ConnectFailed,
        // There is no live socket. Distinct from ConnectFailed: this is the
        // state between a dropped connection and the next retry, and the
        // caller should expect it to clear on its own.
        NotConnected,
        // A send or receive failed, or the peer closed mid-exchange.
        Io,
        // The receiver did not answer inside the deadline. On the control
        // socket this is the common failure and it is not fatal -- the
        // receiver may simply be busy.
        Timeout,
        // The receiver answered with NAK. It understood the request and
        // declined it, so sending the same bytes again will not help.
        Refused,
        // The receiver answered, but with something the protocol does not
        // allow -- a reply of the wrong packet type, a malformed payload.
        Protocol,
        // The request itself is wrong: an unknown record name, a rate this
        // build does not recognise.
        InvalidArgument,
        // Refused locally rather than by the receiver: a raw command while
        // allow_raw_commands is false, a write while in report-only mode.
        NotPermitted,
    };

    Kind kind { Kind::Io };
    std::string message;
    // errno, when there is one. Zero otherwise.
    int code { 0 };
};

const char* to_string(Error::Kind kind);
std::string to_string(const Error& error);

template <typename T>
using Result = std::expected<T, Error>;

// Shorthands, so the call sites read as the thing that went wrong rather than
// as three lines of struct construction.
std::unexpected<Error> not_found(std::string message);
std::unexpected<Error> connect_failed(std::string message, int code = 0);
std::unexpected<Error> not_connected(std::string message);
std::unexpected<Error> io_error(std::string message, int code = 0);
std::unexpected<Error> timeout(std::string message);
std::unexpected<Error> refused(std::string message);
std::unexpected<Error> protocol_error(std::string message);
std::unexpected<Error> invalid_argument(std::string message);
std::unexpected<Error> not_permitted(std::string message);

} // namespace bd992

#endif // BD992_ERROR_H
