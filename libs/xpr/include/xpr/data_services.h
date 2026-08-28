// SPDX-License-Identifier: GPL-3.0-or-later
//
// The NAI data services: one UDP socket per well-known port.
//
// These do NOT ride the XNL session. They are separate protocols on separate
// ports of the same IP link, so a text message and a channel query share
// nothing but the radio's address. The codecs are mototrbo::nai; this is the
// socket under them.
//
// WHAT IS HERE AND WHAT IS NOT, and why. TMS works against a real radio: send
// a datagram to 4007 and the text appears on the display. The LRRP REQUEST
// builder is deliberately absent -- nine framings were tried against the radio
// and none was answered, most likely because GPS is not enabled in its
// codeplug, so the only honest thing to ship is the receive path. A builder
// known not to work is worse than none: it makes the next person debug the
// radio instead of the request.
//
// Nothing in this file is wired to a node yet.

#ifndef XPR_DATA_SERVICES_H
#define XPR_DATA_SERVICES_H

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mototrbo/nai.h"
#include "xpr/error.h"

namespace xpr
{

// A UDP endpoint bound to one NAI port, talking to one radio.
//
// The local port matters: the radio sends its own datagrams TO the well-known
// port number, so a client that bound an ephemeral port would send fine and
// never hear anything back. Binding the service port is what makes the socket
// bidirectional.
class DataService
{
  public:
    static Result<DataService> open(std::string radioHost, std::uint16_t port);

    ~DataService();

    DataService(const DataService&) = delete;
    DataService& operator=(const DataService&) = delete;
    DataService(DataService&& other) noexcept;
    DataService& operator=(DataService&& other) noexcept;

    Result<void> send(std::span<const std::uint8_t> datagram);

    struct Datagram
    {
        std::vector<std::uint8_t> bytes;
        std::string sourceAddress;
        std::uint16_t sourcePort { 0 };
    };

    // One datagram, or Error::Kind::Timeout if none arrives in time.
    Result<Datagram> receive(std::chrono::milliseconds timeout);

    std::uint16_t port() const { return mPort; }
    const std::string& host() const { return mHost; }

  private:
    DataService(int fd, std::string host, std::uint16_t port);

    void close();

    int mFd { -1 };
    std::string mHost;
    std::uint16_t mPort { 0 };
};

// Send a text message to the radio. Convenience over DataService opened on
// mototrbo::nai::kPortTms.
Result<void> send_text(DataService& service, std::string_view text, std::uint8_t sequence = 1,
                       bool requiresAck = true);

} // namespace xpr

#endif // XPR_DATA_SERVICES_H
