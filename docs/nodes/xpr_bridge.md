---
title: xpr_bridge
parent: Nodes
redirect_from: /xpr.html
---

# xpr_bridge

## Overview

A Motorola MOTOTRBO handheld or mobile radio on the vehicle's USB, publishing
what it is doing as Cap'n Proto messages and exposing its channel as a zenoh
service. The node is the top of three layers:
[mototrbo](../libs/mototrbo.html) is the protocols (XNL framing and
authentication, XCMP commands, the on-air control plane, the NAI data-service
codecs), [xpr](../libs/xpr.html) is TCP, the session state machine,
reconnection and the typed queries, and `nodes/xpr_bridge` maps those onto
schemas and reads the YAML. Where the protocol knowledge came from and the
defects the handshake encodes are in the
[design notes](../design/mototrbo.html).

Validated against an XPR 5550 (model `M28TRN9WA1AN`, firmware
`R02.10.00.0001`). The protocol is the whole MOTOTRBO family's, but every
"[CONFIRMED on hardware]" in the source means that radio.

Plugging the radio into USB brings up an RNDIS Ethernet interface. The radio
answers on `192.168.10.1`, and control is an ordinary TCP session to port
`8002`. Nothing in this stack uses libusb; the OS network stack does the USB
part. Port `8003` is an optional secure session, closed on the radio this was
built against and not implemented.

## Running it

```bash
./build/nodes/xpr_bridge/xpr_bridge --config configs/xpr/xpr.yaml --probe
./build/nodes/xpr_bridge/xpr_bridge --config configs/xpr/xpr.yaml
```

| Option | |
| --- | --- |
| `--config <file>` | The YAML below. |
| `--probe` | Connect, complete the authentication handshake, print what the radio calls itself and which channel it is on, and exit without publishing. |
| `--debug` | Verbose logging. |

`--probe` is the first thing to run because it separates "the link is up"
from "the node is misconfigured".

`radio` names the `host` and `port`, a `connect_timeout_ms`, a
`reply_timeout_ms` that is a deadline for a whole exchange rather than a
budget per frame (so a radio pushing display broadcasts cannot stretch it),
and `reconnect_backoff_ms`, tried in order with the last value repeating.
Nothing sleeps on the backoff: a failed attempt schedules the next one, so the
node keeps serving while the radio is unplugged.

`publish` sets `topic_prefix` (default `nodes/xpr`), an optional `status_key`
(default `<topic_prefix>/status`), `status_interval_ms`, `publish_display`
and `publish_unknown_broadcasts`. Leave both of the last two on: the channel
names live in the codeplug, which this node does not read, so the display is
the only place a name can be had, and a radio emitting a broadcast this build
has never seen is otherwise indistinguishable from one that has gone quiet.

`control.allow_channel_change` gates the `set_channel` service.

{: .warning }
Everything this node does is read-only except `set_channel`, and that is off
by default: it moves the radio off the channel whoever is carrying it is
listening to. The node reads no codeplug, keys no transmitter and sends no
tuning command. Set `control.allow_channel_change: true` only for a radio the
vehicle owns.

## Topics

| Topic | Schema | |
| --- | --- | --- |
| `<prefix>/channel` | `XprChannel` | Zone and channel, with the counts. Published when the radio says it moved, not on a timer. |
| `<prefix>/display` | `XprDisplay` | The radio's own four display lines, decoded. |
| `<prefix>/broadcast` | `XprBroadcast` | The broadcasts this build does not decode, as bytes. |
| `<prefix>/status` | `XprRadioStatus` | The session, the counters, and the radio's identity. |

The display topic is where channel names come from. The names live in the
codeplug, which this build deliberately does not read, so the radio's own
screen is the only place a human-readable channel name can be had at all.

Nothing in a service publishes. A channel change reaches the bus through the
radio's own `0xB40D` broadcast, picked up by the node's loop like any other,
so the topic reports what the radio did rather than what a service asked for,
and every publisher stays on one thread, which is what `ZenohPublisher`
requires.

## Health

`nodes/xpr/health` ([NodeHealth](../libs/node_health.html)), once a second and
at once on any change:

| Check | Not ok when |
|---|---|
| `radio` | not connected to the radio (fault) |

`inspect health` prints them.

## Services

| Service | |
| --- | --- |
| `<prefix>/get_channel` | Where the radio is. `refresh` asks the radio rather than answering from the last broadcast; it costs a round trip. |
| `<prefix>/set_channel` | `op` is `up`, `down` or `select`; `select` takes `zone` and `channel`. Refused unless `control.allow_channel_change` is set. |
| `<prefix>/get_identity` | Model, serial, firmware, TANAPA, DMR id. |

Changing the channel is stepping. The radio has a direct-select operation,
and on an XPR 5550 it is accepted and inert: it returns success, echoes the
unchanged zone and channel, and does so even for a channel that does not
exist. So `select` steps with channel-up until the radio reports the target,
bounded by the zone's channel count, and gives up with an error if a step does
not move the radio. The zone must be the one the radio is already on: zone
cannot be changed over this link at all, and a request for another one is
refused rather than approximated by stepping in the hope of crossing a
boundary.

## Tests

```bash
ctest --test-dir build -L xpr
```

| Target | Labels | Proves |
|---|---|---|
| `xpr_test_config` | `xpr unit` | The YAML config without a radio. |
| `xpr_test_publishers` | `xpr unit` | The channel and identity mappings shared by the topics and the service replies: zone and channel are adjacent small integers, and the four identity strings all look alike. |

## Troubleshooting

**`ping 192.168.10.1` does not answer.** The problem is the RNDIS interface,
not anything in this node. Check that the interface came up when the radio
was plugged in.

**`--probe` connects and then times out.** The handshake is not completing.
The five ways that happens silently are in the design notes; on a radio
other than an XPR 5550 the first suspect is a `CONN_REQUEST` variant this
build has not seen.

**`set_channel` is refused.** `control.allow_channel_change` is off, which is
the default, or the request named a zone other than the current one.

**`select` gives up part way.** A step did not move the radio. The library
steps channel-up until the radio reports the target, bounded by the zone's
channel count, and stops with an error rather than looping.

**Channel names are missing.** They are not on `<prefix>/channel` and never
will be; read `<prefix>/display`, and check `publish_display` is on.

**A busy broadcast topic.** The radio is emitting `0xB4xx` broadcasts this
build does not decode. The bytes are on `<prefix>/broadcast`.
