# Motorola MOTOTRBO

A handheld or mobile radio on the vehicle's USB, publishing what it is doing
as Cap'n Proto messages and exposing its channel as a zenoh service.

Three pieces:

| | |
| --- | --- |
| `libs/mototrbo` | The protocols: XNL framing and authentication, XCMP commands, the on-air control plane, the NAI data-service codecs. No sockets, no threads — and `constexpr`, so a wrong offset is a build error. |
| `libs/xpr` | TCP, the session state machine, reconnection, and the typed queries a node needs. |
| `nodes/xpr_bridge` | The process: YAML in, topics and services out. |

The split is the one `gsof` makes from `bd992`: bytes on one side, transports on
the other. `xpr` depends on `mototrbo`; `mototrbo` depends on nothing.

Validated against an **XPR 5550** (model `M28TRN9WA1AN`, firmware
`R02.10.00.0001`). The protocol is the whole MOTOTRBO family's, but every
"[CONFIRMED on hardware]" in the source means that radio.

## The link is a network, not a serial port

Plugging the radio into USB brings up an **RNDIS Ethernet interface**. The radio
answers on `192.168.10.1`, and control is an ordinary TCP session to port
**8002**. Nothing in this stack uses libusb — the OS network stack does the USB
part. If `ping 192.168.10.1` does not answer, the problem is the interface and
not anything in here.

Port 8003 is an optional *secure session*. It is closed on the radio this was
built against, the plaintext session is what the vendor's own client uses
(protocol type 0, no TLS on that path), and 8003 is not implemented.

## Bringing one up

```bash
./build/nodes/xpr_bridge/xpr_bridge --config configs/xpr/xpr.yaml --probe
```

`--probe` connects, completes the authentication handshake, prints what the
radio calls itself and which channel it is on, and exits without publishing
anything. It is the first thing to run because it separates "the link is up"
from "the node is misconfigured".

Then run it without `--probe`.

## What it puts on the bus

| Topic | |
| --- | --- |
| `<prefix>/channel` | Zone and channel, with the counts. Published when the radio says it moved, not on a timer. |
| `<prefix>/display` | The radio's own four display lines, decoded. |
| `<prefix>/broadcast` | The broadcasts this build does not decode, as bytes. |
| `<prefix>/status` | The session, the counters, and the radio's identity. |

| Service | |
| --- | --- |
| `<prefix>/get_channel` | Where the radio is. `refresh` asks the radio rather than answering from the last broadcast. |
| `<prefix>/set_channel` | `up`, `down`, or `select`. Refused unless `control.allow_channel_change` is set. |
| `<prefix>/get_identity` | Model, serial, firmware, TANAPA, DMR id. |

**The display topic is where channel names come from.** The names live in the
codeplug, which this build deliberately does not read (below), so the radio's
own screen is the only place a human-readable channel name can be had at all.

**Nothing in a service publishes.** A channel change reaches the bus through the
radio's own `0xB40D` broadcast, picked up by the node's loop like any other, so
the topic reports what the radio *did* rather than what a service *asked for* —
and every publisher stays on one thread, which is what `ZenohPublisher`
requires.

## Changing the channel is stepping, and that is not a shortcut

The radio has a direct-select operation. On an XPR 5550 it is **accepted and
inert**: it returns success, echoes the unchanged zone and channel, and does so
even for a channel that does not exist. Any zone other than the current one is
rejected outright.

So `selectChannel` steps with channel-up until the radio reports the target,
bounded by the zone's channel count, and gives up with an error if a step does
not move the radio. A zone change is refused rather than approximated — zone
cannot be changed over this link at all, and stepping in the hope of crossing a
boundary would be a guess with somebody's radio.

`control.allow_channel_change` is **off by default** for the same reason. Every
other thing this node does is read-only.

## Five defects the handshake encodes

The protocol was reconstructed from a capture and from the radio's own DLLs, and
then run against the radio — which corrected five things a capture could never
have shown, because a capture only ever shows traffic that *worked* and a
wrong version merely produces silence. They are why the session code looks the way it does, and
`xpr_test_radio`'s fake radio enforces every one:

1. **`CONN_REQUEST` is twelve bytes, not ten**, with the device type at +2 and
   the authentication response at +4. The short form — address then response,
   the obvious reading — is silently dropped. No reply, no error.
2. **The assigned address is at `CONN_REPLY`+2, not +0.** Latent on this radio,
   which does not validate our source address, which is exactly why a capture
   never showed it.
3. **The data-message flags counter must advance.** The radio dedupes on it;
   held at zero, every message after the first is discarded.
4. **Unacknowledged delivery must be selected** in `CONN_REQUEST`'s flags.
   Without it the radio expects an ACK for each of its own messages and
   retransmits five times when it does not get one — so every query returns the
   *previous* query's answer. That reads as a decoding bug and is not one.
5. **Replies correlate on the transaction id, not the opcode.** Several
   distinct queries share one opcode: `0x000E` selects the item with a payload
   byte, so model, serial and DMR id all reply `0x800E`.

## Deliberately absent

**The codeplug.** Reading, decoding and the field schema for it are out of
scope here. This node reports what the radio is *doing*, not how it is
*configured*.

**Anything that transmits.** There is no PTT command, no `Transmit` opcode, and
no RF tuning. `libs/mototrbo`'s `xcmp.h` carries an explicit list of the
destructive opcodes that exist on the radio and are deliberately not
implemented — factory reset, flash erase, boot mode, the codeplug writes. Do not
sweep that space to "complete" the enum.

**An LRRP request builder.** Nine framings were tried against the radio and none
was answered, most likely because GPS is not enabled in its codeplug. The
receive and decode path is there; the request half is not, because a builder
known not to work makes the next person debug the radio instead of the request.

**Record and replay.** No pcap, no session capture.

## The data services are in the library, not the node

`libs/mototrbo/nai.h` and `libs/xpr/data_services.h` carry the text messaging,
location and registration codecs and a UDP endpoint for them. TMS works against
a real radio; ARS is untested and LRRP unanswered. Nothing is wired to a node
yet — when it is, note that these do **not** ride the XNL session. They are
separate protocols on separate UDP ports of the same IP link, sharing only the
radio's address.

## Where the tests get their authority

`libs/mototrbo/tests/golden/hardware_vectors.h` holds bytes the radio actually
sent or accepted, and most of the assertions against them are `static_assert`s —
the same argument `libs/gsof` makes, and it applies harder here because this
protocol is reverse-engineered. A vector written from the same reading of the
protocol as the parser agrees with the parser precisely where both are wrong.

The authentication pairs are the strongest of them: the radio rejects a wrong
TEA response with an all-zero reply, so a live challenge/response pair proves
the key and the cipher rather than proving self-consistency.

Two things in the tree are **synthetic and labelled as such**: the framing of a
`RadioStatus` reply (`<result><item><value>`) comes from the vendor client's own
decoder rather than from a captured reply, so the vectors wrap real values — this radio's DMR id and model number — in that
framing. `parse_status` checks the echoed item against the one requested for
exactly this reason: if the layout is wrong on hardware it reports a mismatch
instead of returning a value read one byte off, which is silent.
