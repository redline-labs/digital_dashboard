// SPDX-License-Identifier: GPL-3.0-or-later
//
// A ByteStream over TCP.
//
// Only ever a client: the devices here (a GNSS receiver, a radio) are TCP
// servers and the node connects to them, so nothing leaves a device until
// something attaches.
//
// Address resolution is AF_UNSPEC, so a device reachable over IPv6 works
// without a second code path, and every address getaddrinfo returns is tried
// in turn before giving up.

#ifndef BYTE_STREAM_TCP_STREAM_H
#define BYTE_STREAM_TCP_STREAM_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "byte_stream/byte_stream.h"
#include "byte_stream/error.h"

namespace byte_stream
{

class TcpStream final : public ByteStream
{
  public:
    // Resolve `host` and connect to `port`, giving up after `connectTimeout`.
    //
    // The timeout is why the connect is non-blocking: a device that is
    // powered off but whose address still routes leaves a blocking connect()
    // sitting for the kernel's own timeout, which on Linux is over two
    // minutes. A reconnect loop built on that cannot be interrupted promptly.
    static Result<std::unique_ptr<TcpStream>> connect(const std::string& host, std::uint16_t port,
                                                      std::chrono::milliseconds connectTimeout);

    ~TcpStream() override;

    bool sendAll(std::span<const std::uint8_t> data) override;
    ssize_t recvSome(std::span<std::uint8_t> out, unsigned timeoutMs) override;
    bool isOpen() const override;
    void close() override;

    // The address actually connected to, for logging. A host name with several
    // A records is otherwise indistinguishable in a log from one that resolved
    // to the wrong machine.
    const std::string& peer() const { return mPeer; }

  private:
    TcpStream(int fd, std::string peer);

    int mFd { -1 };
    std::string mPeer;
};

} // namespace byte_stream

#endif // BYTE_STREAM_TCP_STREAM_H
