---
title: grayhill_keypad
parent: Nodes
---

# grayhill_keypad

## Overview

`grayhill_keypad` puts a Grayhill 3K CANopen keypad on the bus: button state
in, indicator LEDs and brightness out. It speaks PDO to the keypad through
[can_bridge](can_bridge.html)'s raw frame topics, takes the keypad to
operational at startup, and confirms the one SDO write it makes (the producer
heartbeat). It never touches the keypad's object dictionary, so it cannot
change the node ID, bit rate or COB-IDs; that is the job of
`grayhill_keypad_reconfigure`, a separate one-shot binary built from the same
directory, whose whole purpose is writing non-volatile memory. Both are built on `libs/canopen` (see [../libs/](../libs/)).

## Running it

The runtime node requires a configuration file and refuses to start without
one, or with one it cannot fully parse.

```bash
./build/nodes/grayhill_keypad/grayhill_keypad \
    --config configs/grayhill_keypad/grayhill_keypad.yaml
```

| Option | Meaning |
|---|---|
| `--config <yaml>` | Required. Start from the shipped file, which documents every key |
| `-v`, `--verbose` | Debug logging, including every TPDO1 as three hex bytes |

### The reconfiguration tool

`grayhill_keypad_reconfigure` reads a desired-state YAML, builds an ordered
plan of SDO, LSS and NMT steps, prints it, and exits. Writing anything requires
`--apply`; the dry run is the default, and `--dry-run` wins when both are
given.

```bash
# Print the plan. Sends nothing.
./build/nodes/grayhill_keypad/grayhill_keypad_reconfigure \
    --config configs/grayhill_keypad/reconfigure.example.yaml

# Apply it against the in-process simulated keypad.
./build/nodes/grayhill_keypad/grayhill_keypad_reconfigure \
    --config configs/grayhill_keypad/reconfigure.example.yaml --apply

# Apply it to real hardware through can_bridge.
./build/nodes/grayhill_keypad/grayhill_keypad_reconfigure \
    --config my_keypad.yaml --apply --transport zenoh --single-node-bus
```

| Option | Meaning |
|---|---|
| `--config <yaml>` | Required. See `configs/grayhill_keypad/reconfigure.example.yaml` |
| `--apply` | Write. Without it the plan is printed and nothing is sent |
| `--transport stub\|zenoh` | `stub` (the default) is a simulated keypad in this process; `zenoh` sends through can_bridge |
| `--single-node-bus` | Assert the keypad is the only CANopen device attached. Required for any plan that moves the node ID or bit rate, because LSS is a broadcast |
| `--stub-node-id`, `--stub-bitrate` | Where the simulated keypad sits: `10` and `250000` by default, Grayhill's factory settings |
| `--tx-key`, `--rx-key` | Frame topics for the zenoh transport, default `vehicle/can0/tx` and `vehicle/can0/rx` |
| `--eds <path>` | The device EDS; defaults to the copy of `eds/grayhill/DS401_3K_C.eds` shipped with the build |

Before writing, the tool reads `0x1018:01` to prove a keypad answers at the
configured address, so a wrong bit rate or node ID is reported as itself rather
than as a failed first write. Exit codes: `0` done and every write read back,
`1` usage or a config the EDS forbids, `2` no keypad answered, `3` the device
refused a write or did not return after reset, `4` a read-back mismatch.

{: .warning }
As of 2026-09-14 the source marks `libs/canopen`'s zenoh transport as untested
against hardware, and the stub transport is the one with tests behind it. The
warning the tool prints under `--transport zenoh`, that nothing in the tree
terminates the frame topics, predates can_bridge.

### Running without hardware

The reconfigure tool's stub transport runs the whole plan against a simulated
keypad and needs no bus at all. The runtime node has no simulator; with
can_bridge on a `virtual:bench` channel it starts and advertises its topics,
which checks the wiring of topics and services but not decoding.

## Configuration

Unknown keys anywhere in the file are an error, so a typo cannot pass as a
setting that does not work.

| Key | Default when absent | Meaning |
|---|---|---|
| `keypad.node_id` | `10` | CANopen node ID, `1..127`. Grayhill ships at 10; a keypad that has been through MoTeC's PDM Manager is usually elsewhere |
| `keypad.drive_nmt` | `true` | Take the keypad to operational at startup and back to pre-operational at exit. Off when another CANopen master owns the bus |
| `keypad.heartbeat_ms` | `0` | Producer heartbeat written over SDO at startup. `0` means leave the keypad's own setting alone, not disable |
| `keypad.startup_delay_ms` | `500` | Wait for zenoh peering before the first frame goes out |
| `zenoh.rx_key`, `zenoh.tx_key` | `vehicle/can0/rx`, `vehicle/can0/tx` | can_bridge's frame topics for the channel the keypad is on |
| `zenoh.topic_prefix` | `nodes/grayhill_keypad` | Where this keypad's topics and services live; give a second keypad its own |
| `brightness.indicator` | `255` | Button LED brightness, `1..255`. Zero is outside the device's range and is rejected at load |
| `brightness.backlight` | `0` | Panel backlight, `0..255` |

The reconfigure YAML follows the opposite rule: every key under `target` is
optional and an absent key means leave that object alone, while
`current.node_id` and `current.bitrate` are both required.
`motec_compatible: true` sets the two values PDM Manager checks for
(`0x1800:02` and `0x2010:02`, both `0xFE`) and conflicts with an explicit
different value for either; `store` and `reset_after` default to `true`.

## Topics

With the default prefix:

| Topic | Schema | Contents |
|---|---|---|
| `nodes/grayhill_keypad/buttons` | `GrayhillButtons` | Buttons 1..24 as three bytes, one bit each, on every TPDO1 |
| `nodes/grayhill_keypad/status` | `GrayhillStatus` | NMT state from the last heartbeat, boot-up count, last emergency code; on every state change and emergency |

## Services

| Key | Request | Response | Effect |
|---|---|---|---|
| `…/set_indicator_brightness` | `GrayhillSetIndicatorBrightnessRequest` | `GrayhillSetIndicatorBrightnessResponse` | `value` `1..255`; `backlight` says whether the other channel is kept or zeroed |
| `…/set_backlight_brightness` | `GrayhillSetBacklightBrightnessRequest` | `GrayhillSetBacklightBrightnessResponse` | `value` `0..255`; `indicator` as above |
| `…/set_indicators` | `GrayhillSetIndicatorsRequest` | `GrayhillSetIndicatorsResponse` | Up to eight bytes of LED bits; fewer than eight leaves the rest off |

Both brightness channels travel in one RPDO2 frame, so each service sends the
whole frame with the other channel carried over from what was last sent. Every
response carries `ok` and an `error` text naming what was out of range. On
SIGINT or SIGTERM the node darkens the indicators and returns the keypad to
pre-operational before exiting.

## Troubleshooting

**`buttons` never publishes.** The topic is advertised as soon as the node
starts, so `inspect list` proves nothing. `inspect hz vehicle/can0/rx` says
whether any frames arrive at all: none means can_bridge is not running, or the
keypad is on a channel other than the one `rx_key` names. Frames arriving but
nothing decoding means the keypad is transmitting on a COB-ID this node is not
listening on: the node derives the PDO identifiers from `node_id` alone, so a
keypad whose node ID was moved by PDM Manager or by the reconfigure tool needs
the matching `node_id` here.

**The bus is empty and `vehicle/can/status` shows error counters climbing.**
That is a bit-rate mismatch presenting as silence. Grayhill ships at 250 kbit/s,
MoTeC-configured keypads run at 1 Mbit/s, and can_bridge's shipped config is
500 kbit/s; all three have to agree before any frame is acknowledged.

**`status` stays `unknown` while buttons work.** The keypad is not producing a
heartbeat, either because MoTeC's configuration disables it or because
`heartbeat_ms` is `0`, which leaves that alone. Not a fault. A `bootCount`
that climbs while the vehicle runs is a fault: the keypad is resetting, which
is supply or wiring.

**Buttons arrive, but the keypad keeps dropping back to pre-operational.**
Another CANopen master on the bus is issuing NMT commands of its own. Set
`drive_nmt: false` and let it own the keypad's state.
