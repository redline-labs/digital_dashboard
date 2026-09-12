// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong when talking to an MTi over a serial port, in a form a
// caller can act on.
//
// Separate from xbus::Error, which describes bytes that did not decode and is
// deliberately a literal type so the parsers can be constexpr. This one
// describes a device and a port, carries a message and an errno, and is never
// used in a constant expression. The kinds are distinguished because the
// responses differ: `NotConnected` means retry once the reader thread reopens
// the port, `Timeout` means the device is there but did not answer, `Refused`
// means it answered with an Error message and sending the same bytes again
// will get the same answer.

#ifndef MTI610_ERROR_H
#define MTI610_ERROR_H

#include <expected>
#include <string>

namespace mti610
{

struct Error
{
    enum class Kind
    {
        // The device node does not exist. On a USB-serial adapter this is the
        // ordinary state between unplugging and replugging, not a fault.
        NotFound,
        // open() or the termios setup failed. A permission error lands here,
        // and on Linux it is nearly always group membership rather than
        // anything about the device.
        OpenFailed,
        // There is no open port. Distinct from OpenFailed: this is the state
        // between a disconnect and the next retry, and it clears on its own.
        NotConnected,
        // A read or write failed, or the port went away mid-exchange.
        Io,
        // The device did not answer inside the deadline.
        Timeout,
        // The device answered with an Error message (MID 0x42). It understood
        // the request and declined it.
        Refused,
        // The device answered, but with something the protocol does not allow
        // -- an acknowledgement for a different message, a malformed payload.
        Protocol,
        // The request itself is wrong: an unknown output name, a rate outside
        // what the protocol can express.
        InvalidArgument,
        // Refused locally rather than by the device: a write while in
        // report-only mode.
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
std::unexpected<Error> open_failed(std::string message, int code = 0);
std::unexpected<Error> not_connected(std::string message);
std::unexpected<Error> io_error(std::string message, int code = 0);
std::unexpected<Error> timeout(std::string message);
std::unexpected<Error> refused(std::string message);
std::unexpected<Error> protocol_error(std::string message);
std::unexpected<Error> invalid_argument(std::string message);
std::unexpected<Error> not_permitted(std::string message);

} // namespace mti610

#endif // MTI610_ERROR_H
