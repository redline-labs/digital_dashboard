---
title: can_socketcan
parent: Libraries
---

# can_socketcan

## Overview

SocketCAN, the kernel's own CAN stack, one channel per network interface. It
is built on every platform, not only Linux. Two of the three source files are
pure byte manipulation, the frame layouts and the netlink messages that set a
bit rate, and those are the parts most able to be silently wrong, so they are
compiled and tested on a developer's macOS box rather than only on the target.
The socket layer itself is behind `#if defined(__linux__)`, and off Linux the
backend still exists but reports every channel as unsupported. That keeps a
node's channel list meaning the same thing everywhere.

It implements `can::Backend` from [can](can.html) and is registered by
[can_backends](can_backends.html). It does not shell out to `ip`: doing so
would put a parser for another program's output in the path of every
configuration change and need `ip` to exist on the target, so the RTM_NEWLINK
message is built here as bytes. The node that opens it is
[can_bridge](../nodes/can_bridge.html).

## Public headers

| Header | |
| --- | --- |
| `can_socketcan/socketcan_backend.h` | `is_available()` and `make_socketcan_backend()`. |
| `can_socketcan/socketcan_frame.h` | `encode_frame` and `decode_frame` between `helpers::CanFrame` and the kernel's 16- and 72-byte layouts; the EFF, RTR and ERR flag constants. |
| `can_socketcan/netlink.h` | `LinkRequest` and `encode_link_request` (bit rate, up/down, listen-only, FD), `decode_ack`, `encode_link_query`, `decode_link_state` into `LinkState`, `align4`, the kernel's attribute constants. |

## Using it

Link the CMake target `can_socketcan`, or `can_backends`. Channels are named
`socketcan:<interface>` with no channel suffix, because a SocketCAN device is
a channel:

```cpp
can::OpenOptions open;
open.bitrate.nominalBps = 500000;
open.listenOnly = true;
auto channel = registry.open("socketcan:can0", open);
if (!channel && channel.error().kind == can::Error::Kind::PermissionDenied) {
    // the socket works unprivileged; setting the bit rate does not
}
```

## Behaviour worth knowing

Two things need privileges and it is worth knowing which. Opening a `CAN_RAW`
socket and reading and writing frames needs nothing special. Changing the bit
rate or bringing the interface up and down is a link-layer change and needs
`CAP_NET_ADMIN`, because it is the same operation as `ip link set`. A node that
only reads and writes runs unprivileged; reconfiguration comes back as
`PermissionDenied` rather than half-applying.

Off Linux `enumerate()` finds nothing and `open()` fails with `Unsupported`,
not `NotFound`, with a message that names Linux: the interface is not missing,
the whole kernel subsystem is. `is_available()` says which case you are in
before you report "no interfaces found".

A `CAN_RAW` read returns either 16 or 72 bytes, and the size is the only thing
that says whether it is a classic or an FD frame. Both put the identifier in a
32-bit word with three flag bits above it: bit 31 EFF, bit 30 RTR, bit 29 ERR.
Masking with the wrong constant turns a 29-bit identifier into a different
11-bit one, and missing the error bit turns a bus-off report into traffic from
a device that is not there. Neither fails; both produce plausible nonsense
downstream, which is why the conversion is hand-written and tested off Linux.

The kernel splits tseg1 into a propagation segment and a phase segment where
the controller and `can::BitTiming` treat it as one number. The split is
arbitrary as far as the bus is concerned, so everything goes in `phase_seg1`
and `prop_seg` stays zero. Netlink pads everything to four bytes; a message
the kernel cannot parse is a message the kernel ignores without complaint,
and building it wrong looks exactly like a bit rate that did not take effect.

{: .note }
On Linux the attribute constants are the kernel's own, from its headers. Off
Linux the literals stand in so the encoder compiles and its padding and
nesting can be tested; the tests that run there check the shape of a message,
not which attributes it names. The numbers that matter are the ones on the
Linux side of the `#if`.

The backend reads state back rather than believing its own requests. Without
`CAP_NET_ADMIN` every write is refused, and a channel that trusted its requests
once published "up at 500 kbit/s" for an interface that was down and
unconfigured. An `RTM_GETLINK` query needs no privileges, so the truth is
always available. In `LinkState` an absent attribute is `nullopt`, not zero: an
interface that has never been given a bit rate carries no
`IFLA_CAN_BITTIMING` at all. Unfamiliar attributes are ignored rather than
rejected, since the kernel gains them between versions. `fdCapable` answers
"can this do FD" from the controller's data-timing table; `fdEnabled` answers
"is it doing FD". `busOffCount` comes from the XSTATS block and is cumulative,
so a bus-off that happened and recovered still shows where the instantaneous
state no longer does.

## Tests

```bash
ctest --test-dir build -L socketcan    # can_socketcan_test
```

`can_socketcan_test` is labelled `can`, `socketcan` and `unit`. On the frame
side it round-trips classic, extended, error, FD and remote frames and rejects
bad read sizes. On the netlink side it checks interface-name validation, the
structure and rejections of a link request, the bit-timing encoding, ack
decoding, the structure and rejections of a link query, decoding of a running
interface, that a zero bit rate reads as unset, and that availability is
reported honestly on the platform the test runs on.

The golden in `tests/golden/link_reply.h` is a real `RTM_GETLINK` reply, not a
hand-written one, for the same reason `libs/gsof` captures its records: a
vector authored from the same reading of the layout as the parser agrees with
the parser even where both are wrong. It came off a Linux 7.0 kernel with the
`pcan_usb_pro_fd` driver bound to a PCAN-USB Pro FD, using
`tests/golden/capture.cpp`, with the interface down and never given a bit
rate, which is exactly the case that used to be misreported. The Linux socket
code around these two halves cannot be exercised off Linux at all.
