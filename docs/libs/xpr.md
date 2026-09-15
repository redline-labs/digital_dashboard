---
title: xpr
parent: Libraries
---

# xpr

## Overview

Talking to a MOTOTRBO radio over the network: a `ByteStream` abstraction with
a TCP implementation, the XNL session with its handshake and reconnection,
the typed queries a node needs (channel, counts, status items, identity,
stepping and selecting a channel), and a UDP socket for the NAI data
services. Everything here owns a socket or a mutex; nothing here knows a
field offset. That is what lets [mototrbo](mototrbo.html) be `constexpr` and
checked at compile time, and it is why this target depends on `mototrbo` and
`mototrbo` may never depend on this one.

Named for the radio, as `bd992` is named for its receiver. The XPR 5550 is
what this was validated against; the protocol is the whole MOTOTRBO family's.
It is free of zenoh and capnp: the node,
[xpr_bridge](../nodes/xpr_bridge.html), maps a channel or a display line onto
a schema. The rationale is in the [design notes](../design/mototrbo.html).

## Public headers

| Header | |
| --- | --- |
| `xpr/byte_stream.h` | `ByteStream`: the three methods the session is written against, never a socket. |
| `xpr/tcp_stream.h` | `TcpStream::connect(host, port, timeout)`: a client-only `ByteStream` over TCP. |
| `xpr/radio.h` | `Radio`: the XNL session and the typed queries. `connect`, `channel`, `channelCounts`, `status`, `identity`, `stepChannel`, `selectChannel`, `pump`. |
| `xpr/data_services.h` | `DataService`: a UDP endpoint bound to one NAI port, talking to one radio. Not wired to a node yet. |
| `xpr/error.h` | `xpr::Error`: a connection error with a message and an integer. `NotConnected` clears on its own once the reconnect succeeds. |

## Using it

Link the CMake target `xpr`. `Radio` takes a `StreamFactory` and options, and
is safe to call from a zenoh service thread while the node's own loop pumps
it:

```cpp
xpr::Radio radio([&] { return xpr::TcpStream::connect(host, 8002, connectTimeout); },
                 options);
radio.connect();                       // the full XNL handshake, TEA included

auto where = radio.channel();          // zone and channel
auto who = radio.identity();           // model, serial, firmware, TANAPA, DMR id
auto moved = radio.selectChannel(where->zone, 3);

// The node's loop: read broadcasts the radio pushed, reconnect if needed.
radio.pump(std::chrono::milliseconds(100));
```

## Behaviour worth knowing

One socket carries everything: the handshake, every command, and the
unsolicited broadcasts the radio pushes when its own state changes. `Radio`
owns the socket and serialises access to it with a mutex.

Broadcasts are queued, not dispatched. They arrive interleaved with command
replies, so a query has to read past them, and dropping them there is how the
display goes stale exactly when the channel changes, since a channel change
is a broadcast. `pump` is where a queued broadcast reaches the caller, which
is what keeps every publisher on one thread.

`selectChannel` steps rather than selecting. The radio's direct-select
operation is accepted and inert on an XPR 5550: it returns success, echoes
the unchanged zone and channel, and does so even for a channel that does not
exist. So the library steps with channel-up until the radio reports the
target, bounded by the zone's channel count, and gives up with an error if a
step does not move the radio. A zone other than the current one is refused,
because zone cannot be changed over this link at all and stepping in the hope
of crossing a boundary would be a guess with somebody's radio.

`replyTimeout` is a deadline for the whole exchange rather than a budget per
frame, so a radio pushing display broadcasts cannot stretch it. Nothing
sleeps on the reconnect backoff: a failed attempt schedules the next one.

The data services do not ride the XNL session. `DataService` binds the
well-known port, because the radio sends its own datagrams to that port
number and a client that bound an ephemeral port would send fine and never
hear anything back. TMS works against a real radio: send a datagram to `4007`
and the text appears on the display. ARS is untested, LRRP is unanswered, and
nothing in this file is wired to a node yet. There is no record and replay:
no pcap, no session capture.

The `ByteStream::recvSome` contract is that `0` and `-1` stay distinct. A
caller polling for a reply treats `0` as "not yet" and would spin until its
deadline on a dead link if a closed peer also reported `0`.

## Tests

```bash
ctest --test-dir build -L xpr        # xpr_test_radio, xpr_test_data_services, xpr_test_config
```

`xpr_test_radio` is `unit`: the session end to end against a scripted radio
in memory, including the real TEA handshake, which is why it needs no
network. The fake radio enforces the five defects that only hardware could
find (twelve-byte `CONN_REQUEST`, the address at `+2`, the advancing flags
counter, unacknowledged delivery, correlation on transaction id), so a
regression in any of them fails the test rather than producing silence. It is
the same argument `libs/mti610`'s fake device makes. `xpr_test_data_services`
is `net` rather than `unit` because it binds UDP ports on the loopback
interface. `xpr_test_config` lives in the node and recompiles
`node_config.cpp`.
