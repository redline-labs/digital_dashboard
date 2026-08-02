// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two socket shapes the AirPlay session needs, in one place because the
// details are easy to get subtly wrong and the failure is a phone that cannot
// reach us with nothing logged on either side.
//
// Both bind dual-stack (IPv6 with V6ONLY off): the phone reaches us over an
// IPv6 link-local on the NCM link, but the port has to be usable either way.
// Both let the kernel pick the port, because everything except the RTSP server
// itself is advertised to the phone rather than fixed.
#ifndef AIRPLAY_NET_H_
#define AIRPLAY_NET_H_

#include <cstdint>

namespace airplay::net
{

// Opens a listening TCP socket on an ephemeral port. Returns the fd and writes
// the chosen port, which is what gets advertised. -1 on failure.
int openEphemeralListener(uint16_t& port);

// Binds a dual-stack UDP socket on an ephemeral port. Returns the fd and writes
// the chosen port. -1 on failure.
int openUdpSocket(uint16_t& port);

}  // namespace airplay::net

#endif  // AIRPLAY_NET_H_
