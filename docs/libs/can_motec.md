---
title: can_motec
parent: Libraries
redirect_from: /motec_utc.html
---

# can_motec

## Overview

A MoTeC UTC is a USB-to-CAN dongle: an FTDI FT245BM in front of a classic CAN
controller. It does not speak a register protocol the way the PCAN family
does; it speaks a small command envelope with a CRC, and CAN frames travel as
17-byte records in a block that hangs off the end of that envelope. The same
envelope is used over USB bulk endpoints (the dongle) and over UDP (MoTeC's
network gateways), which is why the codec is separate from any transport.

`libs/can_motec` drives both as a `can::Channel`, so a UTC appears alongside
`socketcan:` and `pcan:` in [can_bridge](../nodes/can_bridge.html) and
anything else built on the channel registry. The protocol is not published;
the reverse-engineered description, what has been verified on hardware and
the measurements are in the
[MoTeC UTC protocol](../design/motec-utc-protocol.html) design note.

## Public headers

| Header | |
| --- | --- |
| `can_motec/motec_gw.h` | The pure codec: `encode_frame`, `decode_frame`, `data_block_length`, `pack_records`, `unpack_records`, `to_can_frame`, `from_can_frame`, the `make_*` builders (`make_mode` carries the bit rate), `bitrate_index`, `FrameReader` for stream reassembly, `strip_ftdi_status`, `make_unlock`. |
| `can_motec/utc_backend.h` | The `can::Backend` that enumerates and opens UTCs and gateways; `MotecOptions`. |

`src/utc_transport.h` is private: the libusb and UDP transports behind the
backend.

## Using it

Link the CMake target `can_motec`, or `can_backends`, which assembles it into
the default registry. Channels are named by string:

```
motec:0                    the first UTC attached
motec:56536                that index if it exists, otherwise that serial
motec:serial=56536         that serial, never an index
motec:udp=192.168.1.40     a network gateway speaking the same protocol, port 29456
motec:udp=[fe80::1%eth0]:29456
```

```cpp
auto registry = can_backends::make_default_registry();
auto channel = registry.open("motec:0", can::OpenOptions { .bitrate = bitrate });
// channel->send(frame), channel->receive(out, timeout), channel->statistics()
```

The index-or-serial fallback is not a convenience. A UTC's serial is a bare
number (the one this was written against is `56536`), so there is no way to
tell an index from a serial by looking at it. A digit string is tried as an
index first and as a serial second, and `serial=` forces the second reading.

The bit rate is set at open. It travels in byte +11 of the Mode command that
every session sends (see the
[design note](../design/motec-utc-protocol.html#the-mode-command-filter-receive-mode-and-bit-rate)).
`set_bitrate()` changes it by opening a new session: Open, Version, Mode and
the subscribe again. Only six rates exist: 1 Mbit/s, and 500, 250, 125, 66.67
and 33.33 kbit/s. Any other rate is refused with `Unsupported` before anything
is sent. The sample point has no field in the protocol, so it is ignored.

{: .warning }
Only 1 Mbit/s has been seen working on hardware. The other five indices come
from CAN Inspector's table and are not yet confirmed. Before this was
understood, every session sent index 0, so a UTC opened by any earlier build
was set to 1 Mbit/s whatever `bitrate:` said.

Nothing reports the rate back, so `bitrate()` is what this session told the
device.

The device can latch into a state where every command on the normal tag,
`Open` included, is answered `status 0x21`. `handshake()` recognises this,
sends `make_unlock()` (an `Open` on tag 0 with a version payload) and reopens;
the log says `the gateway was latched and has been cleared` when it happens.
Nothing a caller does is needed. A node that came up against a latched dongle
used to need a human with physical access, and now does not.

## Behaviour worth knowing

Everything below is a place where guessing would fail silently, so the
backend fails loudly instead.

| | Why |
|---|---|
| A rate outside the six | The Mode command's index has six values; anything else is refused. |
| Read the bit rate back | Nothing in the protocol reports it. |
| Listen-only | No known command. A receive filter is not equivalent: it stops frames being delivered, but the controller still acknowledges them on the bus. An open that asks for it fails. |
| CAN FD | The hardware is classic CAN. |
| Bus state, error counters, bus-off | Nothing in the protocol carries them; `statistics()` leaves the controller state `Unknown` while the link is alive. |
| Remote (RTR) frames | Four bits of the flags byte are not understood and one of them is presumably RTR. Sending one would put a data frame on the bus where a request was meant. |
| More than one acceptance filter | The device accepts exactly one Mode command per session. |
| Receiving a saturated bus | A UTC passes about 8,100 frames/s. Past that it loses frames and damages every 128th record; damaged ones are dropped and counted in `rxDropped`. |
| Leaving the link idle | The session times out after about ten seconds of client silence; the driver sends `Version` every `keepAliveIntervalMs` (`2000`) from the receive thread. |
| Closing a session | `make_close()` builds Close (`0x01`), but the backend does not send it: it is untried on hardware. |

The Mode command's mode byte must be 1 to 3; the backend sends 3, stream
mode, with an all-ones mask, which accepts everything. One Mode per session,
before the Rx subscribe; an `Open` starts a new session.

Standard and extended identifiers are laid out differently on the wire: bit
31 set means extended with the 29-bit identifier in bits 0..28, bit 30 set
means standard with the 11-bit identifier left-aligned in bits 18..28. Both
directions were wrong before this was measured against a second dongle; the
table is in `tests/golden/utc_frames.h` as data.

Inbound, every 64-byte USB packet begins with two FTDI modem-status bytes
that are not part of the stream. `strip_ftdi_status()` removes them before
`FrameReader` sees anything. Frames straddle packet boundaries in both
directions and several small frames arrive in one packet, so the reader
reassembles rather than assuming one read is one frame. The FT245BM is a
fixed 64-byte FIFO part with no baud rate, no latency timer and no flow
control, and this driver deliberately issues none of those control requests;
a driver written against the far more common FT232 would, and this device
does not answer them.

Records arrive in batches, around 60 in one data frame on a busy bus, all
pushed onto the receive queue under one lock before the reader can drain any.
A queue shallower than the largest batch drops frames even on a quiet bus.
The `8192` default holds about two seconds of a saturated bus; anything under
about a hundred is asking for drops.

`send()` keeps at most two transmits unanswered by the device. A third waits
for an answer, for up to `txAnswerTimeoutMs` (`1000`), and then fails without
being written. Writing faster than the device drains has latched a UTC so
badly that only a power cycle cleared it. The cost is throughput: about 570
transmits a second. [can_bridge](../nodes/can_bridge.html) sends from its own
thread, so that wait never holds up a zenoh callback.

Within that window `send()` counts a frame optimistically, and the
acknowledgement arrives on the receive thread and corrects `txFrames` and
`txDropped` there. `status 0x20` on a Tx acknowledgement means
the device did not take the frame. It happens transiently under load and
permanently when nothing on the bus acknowledges: a lone UTC accepts about
four frames and then refuses everything, because the controller is still
retrying the first. There is no transmit-overflow report in the protocol, so
`Statistics::txFrames` counts frames handed to the adapter, not frames that
reached the wire.

A read that fails ends the channel. `running()` goes false, and `receive()` and
`send()` answer `NotFound`, which is what [can_bridge](../nodes/can_bridge.html)
reopens on. It used to log the failure every 100 ms and look like a quiet bus.

After the Rx subscribe the device pushes a data frame roughly every 255 ms on
its own request-id counter, empty ones included. Nothing arriving for
`rxStallTimeoutMs` (`3000`) reports `BusState::Unknown` rather than a
flattering `ErrorActive`. The record timestamp runs at roughly 1,001,800
ticks/s, so it drifts against wall-clock by about 0.18%.

## Tests

```bash
ctest --test-dir build -L motec      # can_motec_test_gw, can_motec_test_channel
```

`can_motec_test_channel` (`net`) runs the channel against a fake gateway on
loopback UDP. It covers the bit rate reaching the Mode command, a rate change
opening a new session, refused rates sending nothing, the two-transmit window,
damaged records being dropped, and a dead link ending the channel. Each of
these was checked by reverting the fix and watching the test fail.

`can_motec_test_gw` is labelled `can`, `motec` and `unit`, and it is built
around `tests/golden/utc_frames.h`: eleven frames captured from a genuine UTC
(pcapng captures of CAN Inspector v1.19 talking to real hardware, via
`motec-gw-sim`), every one of which must decode and re-encode
byte-identically. That is the only part of this library that can be called
confirmed by the tests alone, and hand-written vectors would not do: a vector
written from the same reading of the protocol as the parser agrees with the
parser even where both are wrong. The captures are what established that the
extended-identifier bit lives in bit 31 of the identifier rather than in the
flags byte, that the data block carries no CRC of its own, and that a Tx
acknowledgement's byte count is not a block length.

The rest of the file covers the stream reassembly the captures cannot:
`test_reader_reassembles_split_frames`,
`test_reader_handles_several_frames_in_one_push`,
`test_reader_waits_for_a_split_data_block`,
`test_tx_ack_does_not_swallow_the_next_frame`,
`test_reader_resynchronises_after_damage`, `test_ftdi_status_stripping`,
`test_identifier_layouts_against_the_wire` (the measured table),
`test_builders_match_the_captured_handshake` (the Open and the Mode rebuilt
byte for byte, which pins where the bit rate sits), `test_bitrate_index`,
`test_unlock_addresses_the_management_tag` and
`test_keepalive_defaults_stay_inside_the_device_window`. None of it needs a
dongle; the transports are exercised against real hardware and a UDP gateway
as described in the design note.
