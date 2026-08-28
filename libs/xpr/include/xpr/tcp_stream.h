// SPDX-License-Identifier: GPL-3.0-or-later
//
// A ByteStream over TCP.
//
// The radio presents itself as a USB-RNDIS network adapter: plugging it in
// brings up an Ethernet interface, the radio answers on 192.168.10.1, and
// control is an ordinary TCP connection to port 8002. No libusb is involved --
// the OS network stack does the USB part.
//
// Port 8002 is plaintext and is the primary path: the vendor client opens it
// as protocol type 0, and there is no TLS anywhere on that path. The
// certificate the vendor stack carries protects key material for an OPTIONAL
// secondary session on 8003, which is closed on the radio this was validated
// against.
//
// The connect is non-blocking with a deadline for the reason bd992's is: a
// radio that is powered off but whose address still routes leaves a blocking
// connect() sitting for the kernel's own timeout, which is minutes, and a
// reconnect loop built on that cannot be interrupted promptly.

#ifndef XPR_TCP_STREAM_H
#define XPR_TCP_STREAM_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "xpr/byte_stream.h"
#include "xpr/error.h"

namespace xpr
{

// [CONFIRMED] twice over: the vendor client connects to this address and port
// as protocol type 0, and node-dmr-lib reports the same pair independently.
inline constexpr const char* kDefaultHost = "192.168.10.1";
inline constexpr std::uint16_t kDefaultPort = 8002;

class TcpStream final : public ByteStream
{
  public:
    static Result<std::unique_ptr<TcpStream>> connect(const std::string& host, std::uint16_t port,
                                                      std::chrono::milliseconds connectTimeout);

    ~TcpStream() override;

    bool sendAll(std::span<const std::uint8_t> data) override;
    ssize_t recvSome(std::span<std::uint8_t> out, unsigned timeoutMs) override;
    bool isOpen() const override;
    void close() override;

    // The address actually connected to, for logging.
    const std::string& peer() const { return mPeer; }

  private:
    TcpStream(int fd, std::string peer);

    int mFd { -1 };
    std::string mPeer;
};

} // namespace xpr

#endif // XPR_TCP_STREAM_H
