---
title: can_zenoh
parent: Libraries
---

# can_zenoh

## Overview

CAN onto zenoh and back, in both directions. `BridgedChannel` takes any
`can::Channel` and puts it on a pair of topics: what the channel receives is
published on an rx key as `CanFrame`, and frames published on a tx key are
queued and sent. It records both directions to a PCAN `.trc` file when asked and
fills a `CanBridgeChannelStatus` for a status topic.
[can_bridge](../nodes/can_bridge.html) uses it for adapters, and
[pdm32_sim](../sims/pdm32.html) for the CAN port of the device it simulates,
which is why the simulator's bus is indistinguishable from a real one.

The other direction is the `zenoh:` backend: a `can::Channel` whose `receive()`
reads a bridged bus's rx key and whose `send()` publishes on its tx key. A tool
written against `can::Channel` reaches a bus on another machine, or a simulated
one, as it reaches an adapter plugged in here.

It knows nothing about hardware: opening adapters, bit rates and reopening a
channel that went away are the caller's.

## Public headers

| Header | |
| --- | --- |
| `can_zenoh/bridge_config.h` | `BridgeConfig`: the name, the rx and tx keys, whether each direction is carried, the trace path. No dependencies, so a config parser can include it. |
| `can_zenoh/bridged_channel.h` | `BridgedChannel` (`start`, `stop`, `reattach`, `lost`, `fill_status`) and `ReopenBackoff`. |
| `can_zenoh/zenoh_backend.h` | `make_zenoh_backend()`: the `zenoh:<prefix>` backend. |
| `can_zenoh/tx_queue.h` | `TxQueue`: the bounded queue between the tx subscriber and `send()`. |
| `can_zenoh/trc_recorder.h` | `TrcRecorder`: the recording tap, on a thread of its own. |

## Using it

Link the CMake target `can_zenoh`.

```cpp
can_zenoh::BridgeConfig config;
config.name = "can0";
config.rxKey = "vehicle/can0/rx";
config.txKey = "vehicle/can0/tx";
can_zenoh::BridgedChannel bridge(config, channel);   // any shared_ptr<can::Channel>
bridge.start();
```

and from the far end:

```cpp
can::DefaultRegistryOptions options;
options.includeZenoh = true;
auto bus = can::make_default_registry(options).open("zenoh:vehicle/can0", can::OpenOptions{});
```

## Behaviour worth knowing

The tx subscriber only queues. `send()` can block (a MoTeC UTC waits up to a
second for earlier transmits), and a zenoh callback must not, so one thread per
channel drains the queue in order. A full queue refuses the newest frame and
counts it.

A `zenoh:` channel never receives its own frames back, as on real hardware: the
bridge publishes only what its adapter received. It cannot set a bit rate, which
belongs to whoever owns the adapter, and its statistics count only what passed
through it. The prefix's last segment must not be all digits, because a channel
id reads a trailing `/N` as a channel number.

`zenoh:` is off in `make_default_registry` unless `includeZenoh` is set, so a
bridge cannot be configured to bridge its own topics back onto themselves.

A sample on the rx key whose schema is not `CanFrame` delivers nothing. One
whose `len` claims more than its payload carries is cut to the payload, never
padded.

## Tests

| Target | Proves |
| --- | --- |
| `can_zenoh_test_tx_queue` | The queue's bound, its order and its close. |
| `can_zenoh_test_recorder` | Both directions land in the trace in order; a full queue drops and counts instead of blocking. |
| `can_zenoh_test_reconnect` | A channel whose hardware stopped is reported lost, and one opened in its place carries traffic on the same topic. |
| `can_zenoh_test_transmit` | The tx callback queues rather than waiting on a blocked `send()`. |
| `can_zenoh_test_backend` | `zenoh:` against a bridge, both directions with every flag, and a wrong-schema stream and lying lengths on the rx key. |
