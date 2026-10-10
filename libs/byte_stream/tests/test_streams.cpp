// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streams that cannot be opened say why, by kind; a replay hands its bytes over
// in chunks and ends, or loops.

#include "byte_stream/replay_stream.h"
#include "byte_stream/tcp_stream.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <string>

namespace
{

using namespace std::chrono_literals;
using byte_stream::Error;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// A loopback port that was bound and released, so nothing is listening on it.
std::uint16_t closedPort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

}  // namespace

int main()
{
    const auto missing = byte_stream::ReplayStream::open("/nonexistent/capture.bin", {});
    expect(!missing && missing.error().kind == Error::Kind::NotFound, "a missing capture is NotFound");

    const std::string empty = "/tmp/byte_stream_empty_" + std::to_string(::getpid());
    std::ofstream{empty};
    const auto blank = byte_stream::ReplayStream::open(empty, {});
    expect(!blank && blank.error().kind == Error::Kind::Io, "an empty capture is Io");
    ::unlink(empty.c_str());

    auto replay = byte_stream::ReplayStream::fromBytes({1, 2, 3, 4, 5}, {.chunkSize = 2});
    std::array<std::uint8_t, 16> buffer{};
    expect(replay->recvSome(buffer, 0) == 2, "a replay hands over one chunk at a time");
    expect(replay->recvSome(buffer, 0) == 2 && replay->recvSome(buffer, 0) == 1,
           "and the last chunk is what is left");
    expect(replay->recvSome(buffer, 0) == -1 && !replay->isOpen(),
           "then it is closed, -1 and not 0, so a reader cannot spin on it");

    auto looping = byte_stream::ReplayStream::fromBytes({7, 8}, {.chunkSize = 2, .loop = true});
    expect(looping->recvSome(buffer, 0) == 2 && looping->recvSome(buffer, 0) == 2 && buffer[0] == 7,
           "a looping replay starts again");

    const auto refused = byte_stream::TcpStream::connect("127.0.0.1", closedPort(), 1000ms);
    expect(!refused && refused.error().kind == Error::Kind::ConnectFailed,
           "a port nobody listens on is ConnectFailed");

    const auto nohost = byte_stream::TcpStream::connect("no-such-host.invalid", 1, 1000ms);
    expect(!nohost && nohost.error().kind == Error::Kind::NotFound, "a host that does not resolve is NotFound");

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
