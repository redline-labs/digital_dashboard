// SPDX-License-Identifier: GPL-3.0-or-later
//
// A ByteStream over TCP.
//
// The receiver is configured as a TCP server and we connect to it, so this is
// only ever a client -- there is no listener here. That is the whole point of
// the arrangement: nothing leaves the receiver until something attaches, so a
// vehicle network stays quiet and the node is the only thing that has to be
// running.
//
// Address resolution is AF_UNSPEC, so a receiver reachable over IPv6 works
// without a second code path, and every address getaddrinfo returns is tried
// in turn before giving up.

#ifndef BD992_TCP_STREAM_H
#define BD992_TCP_STREAM_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "bd992/byte_stream.h"
#include "bd992/error.h"

namespace bd992
{

class TcpStream final : public ByteStream
{
  public:
    // Resolve `host` and connect to `port`, giving up after `connectTimeout`.
    //
    // The timeout is why the connect is non-blocking: a receiver that is
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

} // namespace bd992

#endif // BD992_TCP_STREAM_H
