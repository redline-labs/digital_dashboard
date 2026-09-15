---
title: can
parent: Libraries
---

# can

## Overview

The backend-independent half of the CAN stack: the `Channel` interface
everything above the drivers is written against, channel naming, bit-timing
arithmetic, the CAN FD length table, and a loopback backend. Deliberately free
of libusb, sockets and zenoh. Everything here builds and is tested on any
platform, which is what makes the parts most likely to be subtly wrong, bit
timing and FD length coding, checkable without a CAN adapter on the desk.

This target knows nothing about the real backends, and they depend on it:
[can_pcan](can_pcan.html), [can_socketcan](can_socketcan.html),
[can_motec](can_motec.html) and [can_trc](can_trc.html) each implement
`can::Backend`. Assembling a registry out of whichever ones a build has is
[can_backends](can_backends.html), which sits above all of them; putting that
here would make `can` depend on libusb to hand out a loopback channel. The
split is the one `gsof` makes from `bd992`: bytes and arithmetic on one side,
transports on the other. The node that opens channels is
[can_bridge](../nodes/can_bridge.html).

## Public headers

| Header | |
| --- | --- |
| `can/channel.h` | `Channel`: configure, start, stop, `send`, `receive`, `statistics`; `BusState`, `Statistics`, and the threading contract. |
| `can/backend.h` | `Backend` (`enumerate`, `open`), `OpenOptions`, and `Registry`, which a caller populates itself. |
| `can/channel_id.h` | `ChannelId` and `parse_channel_id`: `socketcan:can0`, `pcan:0/1`, `virtual:a`; `ChannelInfo` for what a backend found. |
| `can/bitrate.h` | `Bitrate` (what the caller wants), `BitTimingLimits` (what a controller can generate), `BitTiming` (the answer) and `solve_bit_timing`. |
| `can/dlc.h` | The CAN FD length code: `dlc_to_length`, `length_to_dlc`, `is_valid_can_length`, `round_up_can_length`. |
| `can/error.h` | `can::Error` with a `Kind` a caller can act on, `Result<T>` as `std::expected`, and the `not_found`, `busy`, `unsupported` shorthands. |
| `can/virtual_backend.h` | `make_virtual_backend`, `virtual_bus_inject`, `virtual_bus_channel_count`: a named in-process bus with no hardware. |

## Using it

Link the CMake target `can`. Nothing here opens hardware; a caller builds a
`Registry`, adds the backends it wants, and asks it for a channel by string.
The bridge does that through `can_backends`, and a test that wants
deterministic behaviour on a machine with a dongle plugged in adds only the
virtual backend:

```cpp
can::Registry registry;
registry.add(can::make_virtual_backend());

can::OpenOptions open;
open.bitrate.nominalBps = 500000;
auto channel = registry.open("virtual:a", open);

can::virtual_bus_inject("a", frame);   // play some other node on the bus

std::array<helpers::CanFrame, 64> batch;
auto count = (*channel)->receive(batch, can::Duration { 100 });
```

The frame type is `helpers::CanFrame`, shared with every CAN consumer in the
tree; the channel does not define its own.

## Behaviour worth knowing

The threading contract is the thing that bites. `send()` is safe from any
thread while another thread is blocked in `receive()`. Everything else,
`start`, `stop` and `set_bitrate`, must be called from one thread at a time and
not while a receive is in flight. That is what the bridge needs, one reader
thread per channel with senders arriving on zenoh callback threads, and no
more; a fully locked interface would put a mutex in the path of every frame.

`receive()` returning zero is a timeout, and a quiet bus is not an error.
`enumerate()` never fails either: a channel that exists but cannot be opened
comes back with `available` false and `unavailableReason` set, because "your
dongle is held by the kernel driver" is more useful than "no dongle found".

`solve_bit_timing` prefers an exact bit rate over an exact sample point, since
a sample point a few per cent off still communicates and a bit rate a few per
cent off does not. It fails when nothing within the limits comes within 0.5% of
the requested rate, the tolerance CiA 301 allows across a bus. `BitTiming`
carries what the numbers actually produce, `bitrateBps` and
`samplePointPermille`, as opposed to what was asked for, and a caller that
cares should compare them. A sample point of zero in `Bitrate` asks for the
CiA default for that rate, which is what everything else on a vehicle bus will
be using.

{: .warning }
A controller whose bit rate is a few per cent off never wins arbitration, so
the channel transmits nothing, receives nothing and reports no error. That is
how a timing-encoding bug in `can_pcan` asked for 952 kbit/s where 1 Mbit/s
was configured and nothing noticed. When a link carries no traffic and reports
no errors, suspect the timing before the cable.

The CAN FD length code is not a length. DLC 9 to 15 mean 12, 16, 20, 24, 32,
48 and 64 bytes, so a 10-byte payload goes out as 12 with two undefined bytes
and the receiver cannot know the sender meant ten. `is_valid_can_length` lets a
caller refuse rather than pad. With `fd` false the functions clamp to 8.

`ChannelId::toString()` omits a channel suffix of zero, so `pcan:0` and
`pcan:0/0` are the same channel and the shorter form is what gets printed.
`parse_channel_id` reads a trailing `/N` as a channel number only when the
whole segment is digits, which is what lets `trc:/var/log/run.trc` keep its
slashes.

A frame sent on a virtual channel is not delivered back to the sender, matching
a controller without loopback. `virtual_bus_inject` is how a test plays another
node, and it does nothing when no channel is open on that bus, the same as
transmitting onto a bus nobody is listening to. `Registry` is movable but not
copyable: two registries holding the same PCAN backend would each think they
could hand out its channels.

## Tests

```bash
ctest --test-dir build -R can_test_core
```

`can_test_core` is labelled `can` and `unit`. It checks that the standard rates
come out exact at the PCAN family's 80 MHz clock and at other clocks, that the
sample point lands where asked, that the FD data phase solves against its
narrower limits, that impossible rates are refused, the whole DLC table both
ways, channel-id parsing and round-tripping, and the virtual bus: delivery
between channels, isolation between differently named buses, rejection of
malformed frames, delivery across threads, and a registry reporting an unknown
backend name as such. The `can` label is a regex match under `ctest -L`, so
`-L can` also runs `can_trc` and `canopen`.
