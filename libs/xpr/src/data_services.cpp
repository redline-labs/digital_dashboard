// SPDX-License-Identifier: GPL-3.0-or-later

#include "xpr/data_services.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xpr
{

namespace
{

// Big enough for any NAI datagram; they are text messages and location
// reports, not bulk transfer.
constexpr std::size_t kMaxDatagram = 2048;

} // namespace

DataService::DataService(int fd, std::string host, std::uint16_t port) :
    mFd(fd),
    mHost(std::move(host)),
    mPort(port)
{
}

DataService::~DataService()
{
    close();
}

DataService::DataService(DataService&& other) noexcept :
    mFd(other.mFd),
    mHost(std::move(other.mHost)),
    mPort(other.mPort)
{
    other.mFd = -1;
}

DataService& DataService::operator=(DataService&& other) noexcept
{
    if (this != &other)
    {
        close();
        mFd = other.mFd;
        mHost = std::move(other.mHost);
        mPort = other.mPort;
        other.mFd = -1;
    }

    return *this;
}

void DataService::close()
{
    if (mFd >= 0)
    {
        ::close(mFd);
        mFd = -1;
    }
}

Result<DataService> DataService::open(std::string radioHost, std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return connect_failed("socket: " + std::string(std::strerror(errno)), errno);
    }

    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        const int saved = errno;
        ::close(fd);
        return connect_failed("bind :" + std::to_string(port) + ": " + std::strerror(saved), saved);
    }

    return DataService(fd, std::move(radioHost), port);
}

Result<void> DataService::send(std::span<const std::uint8_t> datagram)
{
    if (mFd < 0)
    {
        return not_connected("socket is closed");
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(mPort);
    if (::inet_pton(AF_INET, mHost.c_str(), &address.sin_addr) != 1)
    {
        return not_found("not an IPv4 address: " + mHost);
    }

    const ssize_t sent = ::sendto(mFd, datagram.data(), datagram.size(), 0,
                                  reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (sent < 0)
    {
        return io_error("sendto: " + std::string(std::strerror(errno)), errno);
    }

    return {};
}

Result<DataService::Datagram> DataService::receive(std::chrono::milliseconds timeout)
{
    if (mFd < 0)
    {
        return not_connected("socket is closed");
    }

    pollfd pfd { mFd, POLLIN, 0 };
    const int ready = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (ready == 0)
    {
        return xpr::timeout("no datagram on port " + std::to_string(mPort));
    }
    if (ready < 0)
    {
        if (errno == EINTR)
        {
            return xpr::timeout("interrupted");
        }
        return io_error("poll: " + std::string(std::strerror(errno)), errno);
    }

    std::array<std::uint8_t, kMaxDatagram> buffer {};
    sockaddr_in from {};
    socklen_t fromLength = sizeof(from);
    const ssize_t received = ::recvfrom(mFd, buffer.data(), buffer.size(), 0,
                                        reinterpret_cast<sockaddr*>(&from), &fromLength);
    if (received < 0)
    {
        return io_error("recvfrom: " + std::string(std::strerror(errno)), errno);
    }

    Datagram out;
    out.bytes.assign(buffer.begin(), buffer.begin() + received);

    char text[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
    out.sourceAddress = text;
    out.sourcePort = ntohs(from.sin_port);
    return out;
}

Result<void> send_text(DataService& service, std::string_view text, std::uint8_t sequence, bool requiresAck)
{
    const mototrbo::Result<std::vector<std::uint8_t>> datagram =
        mototrbo::nai::tms::encode_text(text, sequence, requiresAck);

    if (!datagram.has_value())
    {
        return from_protocol(datagram.error(), "encoding a text message");
    }

    return service.send(*datagram);
}

} // namespace xpr
