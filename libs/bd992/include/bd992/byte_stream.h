// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bidirectional byte stream.
//
// Everything above this -- the framer, the page assembler, the record parsers,
// the command exchange -- is written against these three methods and never
// against a socket. That is what lets `--replay` feed a captured file through
// the identical code path the live receiver uses, and what lets the tests
// drive a scripted peer without one.
//
// THE CONTRACT ON recvSome IS THE PART THAT MATTERS, and it is the same one
// apple_usb::ByteStream documents for the same reason: 0 and -1 must stay
// distinct. A caller polling for data treats 0 as "not yet" and would spin
// forever on a dead link if a closed peer also reported 0.
//
// This deliberately does not reuse apple_usb::ByteStream. It is the same three
// methods, but libs/bd992 has no business linking a USB and CarPlay library to
// get an interface, and an interface shared across two unrelated device stacks
// acquires the union of both their needs.

#ifndef BD992_BYTE_STREAM_H
#define BD992_BYTE_STREAM_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/types.h>

namespace bd992
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

} // namespace bd992

#endif // BD992_BYTE_STREAM_H
