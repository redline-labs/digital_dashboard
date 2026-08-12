// SPDX-License-Identifier: GPL-3.0-or-later

#include "bd992/tcp_stream.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bd992
{

namespace
{

std::string describe(const addrinfo& info)
{
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};

    if (::getnameinfo(info.ai_addr, info.ai_addrlen, host, sizeof(host), service, sizeof(service),
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    {
        return "<unprintable>";
    }

    // Brackets around a v6 literal, so a log line can be pasted somewhere that
    // expects host:port.
    if (info.ai_family == AF_INET6)
    {
        return std::string("[") + host + "]:" + service;
    }
    return std::string(host) + ":" + service;
}

// Connect without blocking past `timeout`. Returns the connected fd, or -1
// with errno set.
int connectWithTimeout(const addrinfo& info, std::chrono::milliseconds timeout)
{
    const int fd = ::socket(info.ai_family, info.ai_socktype, info.ai_protocol);
    if (fd < 0)
    {
        return -1;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }

    if (::connect(fd, info.ai_addr, info.ai_addrlen) == 0)
    {
        // Immediate success: a loopback peer, usually, which is exactly what
        // the tests use.
        ::fcntl(fd, F_SETFL, flags);
        return fd;
    }

    if (errno != EINPROGRESS)
    {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }

    pollfd pfd { fd, POLLOUT, 0 };
    const int ready = ::poll(&pfd, 1, static_cast<int>(timeout.count()));

    if (ready == 0)
    {
        ::close(fd);
        errno = ETIMEDOUT;
        return -1;
    }
    if (ready < 0)
    {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }

    // poll() reporting the socket writable does NOT mean the connection
    // succeeded -- a refused connection is also writable. The verdict is in
    // SO_ERROR, and skipping this check yields a socket that appears connected
    // and fails on the first send.
    int soError = 0;
    socklen_t length = sizeof(soError);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length) < 0 || soError != 0)
    {
        const int saved = soError != 0 ? soError : errno;
        ::close(fd);
        errno = saved;
        return -1;
    }

    ::fcntl(fd, F_SETFL, flags);
    return fd;
}

} // namespace

TcpStream::TcpStream(int fd, std::string peer) : mFd(fd), mPeer(std::move(peer))
{
}

TcpStream::~TcpStream()
{
    close();
}

Result<std::unique_ptr<TcpStream>> TcpStream::connect(const std::string& host, std::uint16_t port,
                                                      std::chrono::milliseconds connectTimeout)
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string service = std::to_string(port);

    addrinfo* resolved = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    if (rc != 0 || resolved == nullptr)
    {
        return not_found(host + ": " + ::gai_strerror(rc));
    }

    int lastErrno = 0;
    for (const addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next)
    {
        const int fd = connectWithTimeout(*candidate, connectTimeout);
        if (fd < 0)
        {
            lastErrno = errno;
            continue;
        }

        // Nagle would coalesce a command with whatever follows it, adding up
        // to 40 ms to every configuration exchange for no benefit: these are
        // small, complete messages and there is nothing to batch them with.
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        std::string peer = describe(*candidate);
        ::freeaddrinfo(resolved);
        return std::unique_ptr<TcpStream>(new TcpStream(fd, std::move(peer)));
    }

    ::freeaddrinfo(resolved);
    return connect_failed(host + ":" + service, lastErrno);
}

bool TcpStream::sendAll(std::span<const std::uint8_t> data)
{
    if (mFd < 0)
    {
        return false;
    }

    std::size_t sent = 0;
    while (sent < data.size())
    {
        // MSG_NOSIGNAL rather than a SIGPIPE handler: writing to a receiver
        // that has gone away must return an error, not kill the process. The
        // macOS SDK defines this too, so there is no platform split.
        const ssize_t n = ::send(mFd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }

    return true;
}

ssize_t TcpStream::recvSome(std::span<std::uint8_t> out, unsigned timeoutMs)
{
    if (mFd < 0)
    {
        return -1;
    }

    pollfd pfd { mFd, POLLIN, 0 };
    const int ready = ::poll(&pfd, 1, static_cast<int>(timeoutMs));

    if (ready == 0)
    {
        return 0;
    }
    if (ready < 0)
    {
        // A signal during the wait is not a failure of the connection, and
        // reporting it as one would tear down a healthy socket.
        return errno == EINTR ? 0 : -1;
    }

    const ssize_t n = ::recv(mFd, out.data(), out.size(), 0);
    if (n == 0)
    {
        // Orderly shutdown by the peer. Reported as an error rather than as a
        // timeout, because the caller must stop waiting and reconnect.
        return -1;
    }
    if (n < 0)
    {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0;
        }
        return -1;
    }

    return n;
}

bool TcpStream::isOpen() const
{
    return mFd >= 0;
}

void TcpStream::close()
{
    if (mFd >= 0)
    {
        ::close(mFd);
        mFd = -1;
    }
}

} // namespace bd992
