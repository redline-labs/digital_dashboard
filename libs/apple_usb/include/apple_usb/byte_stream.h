// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef APPLE_USB_BYTE_STREAM_H_
#define APPLE_USB_BYTE_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace apple_usb
{

// A bidirectional byte stream. Lockdown runs first on a bare mux connection and
// then, after StartSession, on a TLS session layered over that same connection;
// the protocol code is identical either side of the switch, so it is written
// against this instead of against a socket.
class ByteStream
{
  public:
    virtual ~ByteStream() = default;

    // Writes the whole buffer. False on error or a closed peer.
    virtual bool sendAll(const uint8_t* data, size_t len) = 0;

    // Reads up to max_len bytes, waiting at most timeout_ms.
    //   >0  bytes read
    //    0  timed out with nothing available
    //   -1  error, or the peer closed
    // The two non-positive cases must stay distinct: a caller polling for data
    // treats 0 as "not yet" and would spin forever on a dead link otherwise.
    virtual ssize_t recvSome(uint8_t* out, size_t max_len, unsigned timeout_ms) = 0;

    virtual void close() = 0;
};

}  // namespace apple_usb

#endif  // APPLE_USB_BYTE_STREAM_H_
