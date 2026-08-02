// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace airplay::net
{

int openEphemeralListener(uint16_t& port)
{
    const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    int off = 0;
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = 0;  // kernel picks
    addr.sin6_addr = in6addr_any;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 4) < 0)
    {
        ::close(fd);
        return -1;
    }

    sockaddr_in6 bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) < 0)
    {
        ::close(fd);
        return -1;
    }
    port = ntohs(bound.sin6_port);
    return fd;
}

int openUdpSocket(uint16_t& port)
{
    const int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    int off = 0;
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = 0;
    addr.sin6_addr = in6addr_any;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return -1;
    }

    sockaddr_in6 bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) < 0)
    {
        ::close(fd);
        return -1;
    }
    port = ntohs(bound.sin6_port);
    return fd;
}

}  // namespace airplay::net
