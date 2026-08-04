// SPDX-License-Identifier: GPL-3.0-or-later
//
// SocketCAN: the kernel's own CAN stack, one channel per network interface.
//
// This is the simple side of the multi-channel question. A SocketCAN device
// *is* a channel -- `can0` is one interface with one controller behind it -- so
// enumerate() lists interfaces and open() makes one socket, with no shared
// device object in between. `socketcan:can0` has no channel suffix because
// there is never more than one.
//
// Two things need privileges and it is worth knowing which. Opening a CAN_RAW
// socket and reading and writing frames needs nothing special. Changing the bit
// rate or bringing the interface up and down is a link-layer change and needs
// CAP_NET_ADMIN, because it is the same operation as `ip link set`. So a node
// that only reads and writes runs unprivileged, and only reconfiguration needs
// more -- which the backend reports as PermissionDenied rather than as a
// mysterious failure.
//
// Builds everywhere. On anything that is not Linux the backend exists but finds
// nothing and refuses to open, which keeps the node's structure identical
// across platforms instead of having the channel list mean different things.
#ifndef CAN_SOCKETCAN_BACKEND_H
#define CAN_SOCKETCAN_BACKEND_H

#include "can/backend.h"

#include <memory>

namespace can::socketcan
{

// Whether this build can actually talk to SocketCAN. False everywhere but
// Linux; worth checking before reporting "no interfaces found", which on macOS
// would be true but misleading.
bool is_available();

std::shared_ptr<Backend> make_socketcan_backend();

} // namespace can::socketcan

#endif // CAN_SOCKETCAN_BACKEND_H
