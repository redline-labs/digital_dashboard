// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bidirectional byte stream.
//
// The XNL session, the handshake and every query are written against these
// three methods and never against a socket, which is what lets the whole
// protocol -- including the real TEA authentication -- be driven by a scripted
// fake radio in a hermetic unit test.
//
// THE CONTRACT ON recvSome IS THE PART THAT MATTERS: 0 and -1 must stay
// distinct. A caller polling for a reply treats 0 as "not yet" and would spin
// until its deadline on a dead link if a closed peer also reported 0.
//
// A third copy of an interface apple_usb and bd992 also declare, and for the
// reason bd992::ByteStream gives: an interface shared across unrelated device
// stacks acquires the union of all their needs, and linking a GNSS library to
// borrow three method signatures is a worse trade than this file.

#ifndef XPR_BYTE_STREAM_H
#define XPR_BYTE_STREAM_H

#include <cstdint>
#include <span>
#include <sys/types.h>

namespace xpr
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

    // Reads up to out.size() bytes, waiting at most timeoutMs.
    //   >0  bytes read
    //    0  timed out with nothing available
    //   -1  error, or the peer closed
    virtual ssize_t recvSome(std::span<std::uint8_t> out, unsigned timeoutMs) = 0;

    virtual bool isOpen() const = 0;

    virtual void close() = 0;
};

} // namespace xpr

#endif // XPR_BYTE_STREAM_H
