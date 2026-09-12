// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bidirectional byte stream.
//
// Everything above this -- the framer, the item walk, the payload parsers, the
// configuration handshake -- is written against these four methods and never
// against a file descriptor. That is what lets `--replay` feed a captured file
// through the identical code path the live device uses, and what lets the
// tests drive a scripted peer without one.
//
// THE CONTRACT ON recvSome IS THE PART THAT MATTERS: 0 and -1 must stay
// distinct. A caller polling for data treats 0 as "not yet" and would spin
// forever on a dead link if a closed peer also reported 0.
//
// This is the same four methods as bd992::ByteStream, and it is deliberately a
// separate interface rather than a link against that library. The reasoning is
// the one bd992/byte_stream.h states about apple_usb::ByteStream: an interface
// shared across two unrelated device stacks acquires the union of both their
// needs, and libs/mti610 has no business pulling in a GNSS library to get four
// virtual functions.

#ifndef MTI610_BYTE_STREAM_H
#define MTI610_BYTE_STREAM_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/types.h>

namespace mti610
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

} // namespace mti610

#endif // MTI610_BYTE_STREAM_H
