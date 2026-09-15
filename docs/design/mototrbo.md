---
title: MOTOTRBO notes
parent: Design notes
---

# MOTOTRBO notes

Where the protocol knowledge came from, what the radio corrected, and why the
session code looks the way it does. The pages this backs are
[xpr_bridge](../nodes/xpr_bridge.html), [mototrbo](../libs/mototrbo.html) and
[xpr](../libs/xpr.html).

## Where the protocol knowledge came from

None of this is published. The protocol was reconstructed from a capture and
from the radio's own DLLs, then run against an XPR 5550 (model `M28TRN9WA1AN`,
serial `511TVMG951`, firmware `R02.10.00.0001`). The XCMP opcodes were
recovered from the vendor's own client, where each is a message class's
declared opcode, so their values are authoritative even where this build has
never sent one. The NAI ports and formats are corroborated across two
independent community implementations, node-dmr-lib (rick51231) and Moto.Net
(pboyd04).

The link is a network, not a serial port. Plugging the radio into USB brings
up an RNDIS Ethernet interface; the radio answers on `192.168.10.1` and
control is TCP to port `8002`. Port `8003` is an optional secure session. It
is closed on the radio this was built against, the plaintext session is what
the vendor's own client uses (protocol type 0, no TLS on that path), and the
certificate the vendor stack carries protects key material for that optional
path only. `8003` is not implemented.

## Five defects the handshake encodes

A capture only ever shows traffic that worked, and a wrong version merely
produces silence. Running the reconstruction against the radio corrected five
things a capture could never have shown. They are why the session code looks
the way it does, and `xpr_test_radio`'s fake radio enforces every one.

`CONN_REQUEST` is twelve bytes, not ten, with the device type at `+2` and the
authentication response at `+4`. The short form, address then response, which
is the obvious reading, is silently dropped. No reply, no error.

The assigned address is at `CONN_REPLY+2`, not `+0`. This was latent on this
radio, which does not validate our source address, which is exactly why a
capture never showed it.

The data-message flags counter must advance. The radio dedupes on it; held at
zero, every message after the first is discarded.

Unacknowledged delivery must be selected in `CONN_REQUEST`'s flags. Without it
the radio expects an ACK for each of its own messages and retransmits five
times when it does not get one, so every query returns the previous query's
answer. That reads as a decoding bug and is not one.

Replies correlate on the transaction id, not the opcode. Several distinct
queries share one opcode: `0x000E` selects the item with a payload byte, so
model, serial and DMR id all reply `0x800E`.

## Why the tests are static_asserts over hardware bytes

`libs/mototrbo/tests/golden/hardware_vectors.h` holds bytes the radio actually
sent or accepted, and most of the assertions against them are
`static_assert`s. It is the same argument `libs/gsof` makes, and it applies
harder here because this protocol is reverse-engineered. A vector written
from the same reading of the protocol as the parser agrees with the parser
precisely where both are wrong.

The authentication pairs are the strongest of them: the radio rejects a wrong
TEA response with an all-zero reply, so a live challenge/response pair proves
the key and the cipher rather than proving self-consistency.

Two things in the tree are synthetic and labelled as such. The framing of a
`RadioStatus` reply (`<result><item><value>`) comes from the vendor client's
own decoder rather than from a captured reply, so the vectors wrap real values
(this radio's DMR id and model number) in that framing. `parse_status` checks
the echoed item against the one requested for exactly this reason: if the
layout is wrong on hardware it reports a mismatch instead of returning a value
read one byte off, which is silent.

## Why changing the channel is stepping

The radio has a direct-select operation. On an XPR 5550 it is accepted and
inert: it returns success, echoes the unchanged zone and channel, and does so
even for a channel that does not exist. Any zone other than the current one is
rejected outright. So `selectChannel` steps with channel-up until the radio
reports the target, bounded by the zone's channel count, and gives up with an
error if a step does not move the radio. A zone change is refused rather than
approximated, because stepping in the hope of crossing a boundary would be a
guess with somebody's radio.

`control.allow_channel_change` is off by default for the same reason. Every
other thing the node does is read-only.

## Why nothing in a service publishes

A channel change reaches the bus through the radio's own `0xB40D` broadcast,
picked up by the node's loop like any other, so the topic reports what the
radio did rather than what a service asked for. It also keeps every publisher
on one thread, which is what `ZenohPublisher` requires. Broadcasts are queued
inside `Radio` rather than dispatched from wherever they were read, because
they arrive interleaved with command replies and dropping them there is how
the display goes stale exactly when the channel changes.

## Deliberately absent

The codeplug: reading, decoding and the field schema for it are out of scope.
The node reports what the radio is doing, not how it is configured. The
consequence is that channel names come only from the display topic.

Anything that transmits: there is no PTT command, no `Transmit` opcode and no
RF tuning. `xcmp.h` carries an explicit list of the destructive opcodes that
exist on the radio and are deliberately not implemented, and the enum must
not be "completed".

An LRRP request builder: nine framings were tried against the radio and none
was answered, most likely because GPS is not enabled in its codeplug. The
receive and decode path is there; the request half is not.

Record and replay: no pcap, no session capture.

## The data services are in the library, not the node

`libs/mototrbo/nai.h` and `libs/xpr/data_services.h` carry the text messaging,
location and registration codecs and a UDP endpoint for them. TMS works
against a real radio; ARS is untested and LRRP unanswered. Nothing is wired to
a node yet. When it is, these do not ride the XNL session: they are separate
protocols on separate UDP ports (`4007`, `4005`, `4001`) of the same IP link,
sharing only the radio's address.
