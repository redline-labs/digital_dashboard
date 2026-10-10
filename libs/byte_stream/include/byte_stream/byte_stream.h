// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bidirectional byte stream.
//
// Everything above this -- framers, parsers, command exchanges -- is written
// against these methods and never against a socket or a tty. That is what lets
// `--replay` feed a captured file through the identical code path the live
// device uses, and what lets the tests drive a scripted peer without one.
//
// THE CONTRACT ON recvSome IS THE PART THAT MATTERS, and it is the same one
// apple_usb::ByteStream documents for the same reason: 0 and -1 must stay
// distinct. A caller polling for data treats 0 as "not yet" and would spin
// forever on a dead link if a closed peer also reported 0.
//
// One copy for the BD992, the MTi and the XPR, which each had their own -- with
// the TCP and replay streams under it -- so a fix to connecting lands in all
// three. It lives here, with no device code, because linking one device's
// library to borrow an interface was the reason for the copies.
// apple_usb::ByteStream stays separate: it is a pointer-and-length API the TLS
// stack is written against.

#ifndef BYTE_STREAM_BYTE_STREAM_H
#define BYTE_STREAM_BYTE_STREAM_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/types.h>

namespace byte_stream
{

class ByteStream
{
  public:
    virtual ~ByteStream() = default;

    ByteStream() = default;
    ByteStream(const ByteStream&) = delete;
    ByteStream& operator=(const ByteStream&) = delete;
    ByteStream(ByteStream&&) = delete;
    ByteStream& operator=(ByteStream&&) = delete;

    // Writes the whole buffer. False on error or a closed peer.
    virtual bool sendAll(std::span<const std::uint8_t> data) = 0;

    // Reads up to out.size() bytes, waiting at most timeout_ms.
    //   >0  bytes read
    //    0  timed out with nothing available
    //   -1  error, or the peer closed
    virtual ssize_t recvSome(std::span<std::uint8_t> out, unsigned timeoutMs) = 0;

    // True while the stream can carry bytes. A stream that has hit an error or
    // reached the end of a replay file reports false and stays that way.
    virtual bool isOpen() const = 0;

    virtual void close() = 0;
};

} // namespace byte_stream

#endif // BYTE_STREAM_BYTE_STREAM_H
