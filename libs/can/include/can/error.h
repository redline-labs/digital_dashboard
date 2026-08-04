// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong when talking to a CAN adapter, in a form a caller can act on.
//
// The kinds are distinguished because the responses differ: `NotFound` means
// check the cable, `PermissionDenied` means the process needs privileges it was
// not given, `Unsupported` means this backend will never do this and retrying
// is pointless, and `Io` means try again. A single opaque error string forces
// every caller to grep the message to decide.
#ifndef CAN_ERROR_H
#define CAN_ERROR_H

#include <expected>
#include <string>

namespace can
{

struct Error
{
    enum class Kind
    {
        // No adapter, or no such channel on the adapter that is there.
        NotFound,
        // The device is there but something else holds it. On Linux a PCAN
        // dongle is claimed by the peak_usb kernel driver at plug-in, so this
        // is the expected answer for the libusb backend on a stock Linux box.
        Busy,
        // Root, CAP_NET_ADMIN, or a udev rule is missing.
        PermissionDenied,
        // This backend cannot do this at all -- SocketCAN on macOS, CAN FD on
        // a classic adapter, a bit rate outside what the controller can
        // generate. Retrying will not help.
        Unsupported,
        // The request itself is wrong: a malformed channel id, a frame whose
        // identifier does not fit its format.
        InvalidArgument,
        // The channel is not in a state where this makes sense -- sending on a
        // stopped channel, changing bit rate while the bus is live.
        InvalidState,
        // A transfer failed, timed out, or came back malformed.
        Io,
        // The device answered, but with something the protocol does not allow.
        Protocol,
    };

    Kind kind { Kind::Io };
    std::string message;
    // errno or a libusb error code, when there is one. Zero otherwise.
    int code { 0 };
};

const char* to_string(Error::Kind kind);
std::string to_string(const Error& error);

template <typename T>
using Result = std::expected<T, Error>;

// Shorthands, so the call sites read as the thing that went wrong rather than
// as three lines of struct construction.
std::unexpected<Error> not_found(std::string message);
std::unexpected<Error> busy(std::string message);
std::unexpected<Error> permission_denied(std::string message);
std::unexpected<Error> unsupported(std::string message);
std::unexpected<Error> invalid_argument(std::string message);
std::unexpected<Error> invalid_state(std::string message);
std::unexpected<Error> io_error(std::string message, int code = 0);
std::unexpected<Error> protocol_error(std::string message);

} // namespace can

#endif // CAN_ERROR_H
