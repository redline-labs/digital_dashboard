---
title: can_backends
parent: Libraries
---

# can_backends

## Overview

Assembles the CAN backends into one registry. It sits above all of them so
that [can](can.html), which they are written against, does not have to know
they exist: putting `make_default_registry` in `can` would make a library that
hands out a loopback channel depend on libusb. It is small enough that the
split costs nothing and keeps the dependency arrows pointing one way.

There is no logic of its own here and nothing that opens hardware. It links
`can`, [can_motec](can_motec.html), [can_pcan](can_pcan.html),
[can_socketcan](can_socketcan.html) and [can_trc](can_trc.html), and a caller
that links this gets all of them. The caller is
[can_bridge](../nodes/can_bridge.html).

## Public headers

| Header | |
| --- | --- |
| `can_backends/registry.h` | `DefaultRegistryOptions`, one options struct per backend plus an `include*` flag each, and `make_default_registry`. |

## Using it

Link the CMake target `can_backends`. The bridge fills the options from its
config and opens channels by the strings in its channel list:

```cpp
can::DefaultRegistryOptions registryOptions;
registryOptions.pcan.detachKernelDriver = config.pcanDetachKernelDriver;
registryOptions.trc.speed = config.trcReplaySpeed;
registryOptions.trc.paced = config.trcReplayPaced;
auto registry = can::make_default_registry(registryOptions);

auto opened = registry.open(channelConfig.device, open);   // "pcan:0/1", "trc:/logs/run.trc/2"
```

## Behaviour worth knowing

Backends are registered in the order PCAN, MoTeC, SocketCAN, virtual, trace.
The order decides only what `Registry::enumerate()` lists first, so real
hardware comes before the loopback and `can_bridge --list` puts the thing that
was plugged in at the top.

The `include*` flags leave a backend out entirely, which is what a test wants
for deterministic behaviour on a machine that happens to have a dongle plugged
in. The per-backend options are backend-wide, not per channel: a trace replay
speed applies to every `trc:` channel opened from this registry, matching how
`PcanOptions` reaches the PCAN backend.

## Tests

None of its own. The one function is exercised by the bridge node's tests and
by every backend's own test target; see the pages for
[can](can.html#tests), [can_pcan](can_pcan.html#tests),
[can_socketcan](can_socketcan.html#tests) and [can_trc](can_trc.html#tests).
