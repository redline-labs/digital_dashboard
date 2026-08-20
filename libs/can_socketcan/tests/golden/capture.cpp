// SPDX-License-Identifier: GPL-3.0-or-later
//
// Regenerates link_reply.h's bytes. Not built: it needs a Linux box with the
// interface attached, and the point of the golden header is that the test does
// not. Build it by hand when capturing a new case -- an interface that is UP
// and configured for CAN FD is the one still missing.
//
//     g++ -std=c++23 -I libs/can_socketcan/include -I libs/can/include \
//         -I libs/helpers/include capture.cpp <libs> -o capture
//     ./capture can0 > reply.bin
#include "can_socketcan/netlink.h"
#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <array>
using namespace can::socketcan;
int main(int argc, char** argv)
{
    int fd = ::socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    sockaddr_nl a{}; a.nl_family = AF_NETLINK;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    auto msg = encode_link_query(argc > 1 ? argv[1] : "can0", 1);
    sockaddr_nl k{}; k.nl_family = AF_NETLINK;
    ::sendto(fd, msg->data(), msg->size(), 0, reinterpret_cast<sockaddr*>(&k), sizeof(k));
    std::array<uint8_t, 8192> reply{};
    ssize_t got = ::recv(fd, reply.data(), reply.size(), 0);
    fwrite(reply.data(), 1, static_cast<size_t>(got), stdout);
    return 0;
}
