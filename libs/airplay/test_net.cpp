// SPDX-License-Identifier: GPL-3.0-or-later
//
// The accepted-socket options the phone's sessions depend on.
//
// TCP_NODELAY is the one worth a test: without it nothing fails, touch reports
// just leave late whenever the phone is slow to acknowledge the previous one,
// which reads as a laggy screen rather than as a socket option.
#include "airplay/net.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

// A client connected to the listener over IPv4 loopback, which the dual-stack
// listener must accept as well as IPv6. -1 on failure.
int connectLoopback(uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool noDelay(int fd)
{
    int value = 0;
    socklen_t len = sizeof(value);
    return ::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &len) == 0 && value != 0;
}

}  // namespace

int main()
{
    using namespace airplay::net;

    uint16_t port = 0;
    const int listener = openEphemeralListener(port);
    expect(listener >= 0, "an ephemeral listener opens");
    expect(port != 0, "the listener reports the port the kernel chose");
    if (listener < 0)
    {
        return EXIT_FAILURE;
    }

    // The control: a plain accept does not get the option, so the check below
    // is measuring acceptNoDelay rather than a platform default.
    {
        const int client = connectLoopback(port);
        expect(client >= 0, "a client connects for the plain accept");
        const int plain = ::accept(listener, nullptr, nullptr);
        expect(plain >= 0, "the plain accept succeeds");
        expect(plain < 0 || !noDelay(plain), "a plain accepted socket has Nagle on (control)");
        if (plain >= 0) ::close(plain);
        if (client >= 0) ::close(client);
    }

    // The accepted socket has Nagle off, and the peer is reported. The listener
    // is IPv6, so an IPv4 client arrives as a v4-mapped address.
    {
        const int client = connectLoopback(port);
        expect(client >= 0, "a client connects");
        sockaddr_in6 peer{};
        const int server = acceptNoDelay(listener, &peer);
        expect(server >= 0, "acceptNoDelay accepts");
        expect(server >= 0 && noDelay(server), "the accepted socket has TCP_NODELAY set");
        expect(peer.sin6_family == AF_INET6, "the peer address is written");
        expect(peer.sin6_port != 0, "the peer port is written");
        if (server >= 0) ::close(server);
        if (client >= 0) ::close(client);
    }

    // No peer requested is fine.
    {
        const int client = connectLoopback(port);
        const int server = acceptNoDelay(listener);
        expect(server >= 0 && noDelay(server), "TCP_NODELAY is set without a peer out-parameter");
        if (server >= 0) ::close(server);
        if (client >= 0) ::close(client);
    }

    // A bad listener fails like accept() rather than handing back a socket.
    expect(acceptNoDelay(-1) == -1, "an invalid listener returns -1");
    {
        const int not_listening = ::socket(AF_INET6, SOCK_STREAM, 0);
        expect(acceptNoDelay(not_listening) == -1, "a socket that is not listening returns -1");
        ::close(not_listening);
    }

    ::close(listener);

    if (failures == 0)
    {
        SPDLOG_INFO("net tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
