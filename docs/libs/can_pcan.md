---
title: can_pcan
parent: Libraries
---

# can_pcan

## Overview

PCAN-USB FD adapters over libusb, without PEAK's driver or their PCBUSB
library. The split inside it is deliberate: `pucan.cpp` is the wire protocol,
command framing, record decoding, bit-timing register packing and FD length
coding, and has no libusb in it at all, so every part of this driver that can
be wrong without hardware attached is testable without hardware attached. The
libusb half is transfers and thread lifetime, and there is not much of it. It
builds on macOS and Linux alike.

It implements `can::Backend` from [can](can.html) and is registered by
[can_backends](can_backends.html); nothing above the registry knows it exists.
It covers the FD-generation adapters only: PCAN-USB FD, PCAN-USB Pro FD,
PCAN-USB X6 and PCAN-Chip USB, which speak uCAN. The classic PCAN-USB speaks an
older register-poking protocol and is not handled. The node that opens it is
[can_bridge](../nodes/can_bridge.html).

## Public headers

| Header | |
| --- | --- |
| `can_pcan/pcan_backend.h` | `PcanOptions` (`detachKernelDriver`, read and write timeouts) and `make_pcan_backend`. |
| `can_pcan/pucan.h` | The uCAN codec: USB ids, endpoints, `FirmwareInfo`, `Opcode`, the `append_*` command builders, `decode_records`, `append_tx_frame`, the flag and option bits. No I/O. |

## Using it

Link the CMake target `can_pcan`, or `can_backends` to get it alongside the
others. Channels are named `pcan:<device>/<channel>`, where the device is an
index or a serial and the channel suffix is omitted when it is 0:

```
pcan:0             the first dongle found, its channel 0
pcan:0/1           that same dongle, channel 1
pcan:LSN00123/1    the dongle with that serial, channel 1
```

The bridge passes one option through from its config:

```cpp
can::DefaultRegistryOptions registryOptions;
registryOptions.pcan.detachKernelDriver = config.pcanDetachKernelDriver;
auto registry = can::make_default_registry(registryOptions);
auto channel = registry.open("pcan:0/1", open);
```

## Behaviour worth knowing

A PCAN-USB Pro FD is one USB device with one pair of bulk endpoints carrying
both channels' traffic interleaved, tagged by a channel index in every record,
and it cannot be opened twice. The backend keeps one device object per USB
interface, reference-counted by the channels handed out of it: opening
`pcan:0/0` creates the device, opening `pcan:0/1` finds the existing one, and
the device closes when the last channel does. One reader thread per device
demultiplexes records into per-channel queues.

The X6 is assumed to be six channels behind three USB interfaces of two, so
channel 4 is interface 2's local channel 0. No X6 has been tried, and PEAK's
Windows driver suggests the assumption is wrong: it binds the X6 as a whole
device and reads a module slot (0 to 2) from the device-info record, which
looks like three two-channel devices. The X6 is not a supported target.

{: .warning }
On Linux the in-tree `peak_usb` driver claims a PCAN adapter at plug-in, and
taking it away destroys the SocketCAN interface it created. The default is to
refuse a device the kernel holds, reported as `Busy`, and
`detachKernelDriver` is opt-in. On Linux the SocketCAN backend is the better
path anyway.

`libs/can_pcan/tests/test_pucan.cpp` says the opcodes come from the mainline
Linux `peak_usb` driver's description of the protocol and have not been
confirmed against hardware. `pucan.h`, written after it, records that they have
since been checked against a PCAN-USB Pro FD running firmware 3.4.4, and that
the warning was right: a real dongle did not answer, and four things were
wrong. Bit timing was encoded without the "counted from zero" decrement on
every field except the prescaler, which asked for 952 kbit/s when 1 Mbit/s was
configured. The timing field masks were narrower than the device's, so a long
tseg1 was truncated into a different bit rate at slower speeds. The standard
acceptance filter was written as one command when it is 64 rows of 32
identifiers, so only identifiers 0x000 to 0x01F could arrive. And
`PUCAN_OPTION_ERROR` was never enabled, so the device sent no error records and
the counters, bus state and bus-off count could not move. Every one of those
failed silently: there is no error path in this protocol for "your timing is
wrong", the controller never wins arbitration, and a bus with a working
cable, dongle and driver carries nothing. Two dongles on one bus is what found
it; see [can_motec](can_motec.html).

A malformed record stops the walk through a bulk transfer rather than being
skipped. The length field is what says where the next record starts, so once
it is wrong there is no way to resynchronise inside the buffer, and guessing
would turn one bad transfer into a stream of plausible nonsense. Everything
decoded before the bad record is still returned.

Opcodes `0x00d` to `0x00f` and `0x011` are the error generator (below) and
the barrier is `0x010`; a command the device does not implement is ignored in
silence. Commands are eight bytes each. A
group of them goes in one transfer of at most 512 bytes (64 on a low-speed
link), terminated by an end-of-collection marker when there is room, and not
padded. The "driver loaded" vendor request is what takes the adapter out of
its idle state; without it nothing answers on the bulk endpoints. Calibration
messages are the device's own timestamp traffic and are switched off, or they
arrive as a stream of records nothing wants: one a second, sent even while no
channel is open, so a session that leaves them on hands the next one a backlog.
Switching them off puts the `0x8000` bit in the USB mask, bytes 6 to 7 of the
clear command, as Linux's `peak_usb` does. The disassembly of PEAK's Windows
driver suggested bytes 4 to 5; a Pro FD ignores that.

**A command group is never split across transfers.** Starting a channel is three
groups: the error generator off, the managed options cleared and the error
counters zeroed, then the
64-row pass-all filter, which fills
a buffer exactly, then the options and the mode. When the filter and the rest went out as one 536-byte
transfer, a PCAN-USB Pro FD on macOS was left in reset. It was silent and
acknowledged nothing, and no error appeared anywhere. `check_command_group()`
is what `send_commands()` enforces, and `build_start_groups()` is unit-tested
against it.

**The data endpoint is read with posted transfers, not timed reads.** Four
bulk reads are kept submitted with no timeout, resubmitted as each completes,
and cancelled only at close. A reader thread runs libusb's event loop until
the last one has come back. The earlier loop of 100 ms synchronous reads
timed out ten times a second on a quiet bus. On macOS each timeout makes
libusb abort the pipe and clear the endpoint's halt, and after a session of
that a Pro FD stopped answering even control requests until it was replugged.
An unplug ends every posted read, and the first to see it tells each open
channel.

## Controller options

The PCAN backend supports every field of `can::ControllerOptions`. They map to
the uCAN option bits PEAK's driver names: self-acknowledge `0x0040`, classic
frames only `0x0010`, ignore BRS frames `0x0080`, start-of-frame timestamps
`0x0020`, and low-latency receive the USB mask's fast-forward bit `0x2000`.
A start clears all five, because the adapter keeps them across sessions, and
then sets the requested ones just before the mode command; with the default
options the filter and option groups are unchanged.

Self-acknowledge was checked on a Pro FD with nothing else acknowledging: off,
the transmit error counter climbs by eight to 128 and every attempt is an ACK
error; on, each frame succeeds and the counter steps back down. The other four
were accepted with classic traffic passing both ways, but a bus with no FD
nodes cannot show what they change. FD non-ISO is not offered: PEAK has an option bit and a separate,
stored ISO-mode command, and the bit's polarity is not settled.

## Error generator

`Channel::set_error_injection()` drives the adapter's error generator, which
destroys frames with six consecutive dominant bits; see [can](can.html) for the
interface. The command layouts come from the description table in PEAK's
Windows driver and from the code there that builds them, and were confirmed on
a Pro FD at 1 Mbit/s against a MoTeC UTC.

| Request | Commands |
| --- | --- |
| `off()` | `0x00f` |
| `next_frame(bit)` | `0x011`, the bit position in the low ten bits of bytes 2 to 3 |
| `targeted(id, bit, destroy, pass, repeat)` | `0x00d` (bit position, then the 29-bit identifier in bytes 4 to 7), then `0x00e` (repeat count in byte 2, destroyed in bytes 4 to 5, passed in 6 to 7) |

The targeted form's two commands go out as two transfers, part 1 first, as
PEAK's driver sends them. PEAK's driver always sends a repeat count of zero,
which means until turned off. Starting a channel turns the generator off first,
in a group of its own, because a session that died with it running leaves it
running on the adapter; stopping a channel that turned it on turns it off.

What the bench showed:

- `next_frame` destroys exactly one frame. The sender retransmits it, so a
  receiver still gets every message.
- `targeted` with destroy 1 and pass 2 passed two frames, destroyed the
  third, and repeated. Passing comes first, and a retransmission counts as a
  frame, so this is one attempt in three, not one message in three.
- An 11-bit identifier matches as written (`0x456`); `0x457` destroyed
  nothing.
- The adapter destroys its own transmissions too. Each costs it eight on its
  transmit error counter, so a steady diet of them takes it error-passive.
- **Passing zero destroys every matching frame** until turned off, and the
  sender goes bus-off.

On a frame from another node, a generated error arrives as an ordinary stuff
error seen while receiving; on the adapter's own frame, as a bit error seen
while transmitting. Nothing in the record marks it as the generator's. The
type byte was the channel in its low nibble, the kind in bits 4 to 6 (0 bit,
2 stuff, 3 ACK, 4 a counter going down) and bit 7 when receiving; the code
byte is not understood. Both are carried raw in `Record::errorTypeByte` and
`errorCodeByte` and logged at trace level.

Not yet known: where the bit position counts from, and whether it includes
stuff bits. Only bit 20 has been tried.

**A start zeroes both error counters** (`0x00a`, both write enables). The
adapter keeps them across sessions, and nothing else brings a receive counter
down but receiving. Before this, channel 1 of the Pro FD tested -- not wired
to anything -- was stuck at REC 136 from some earlier use: error-passive, and
an error-passive transmitter is not charged for a missing ACK, so every ACK
error it reported read REC 136, TEC 0, and nothing ever moved. Zeroed, it
counts exactly as channel 0 does.

## Tests

```bash
ctest --test-dir build -L pcan       # can_pcan_test_pucan
```

`can_pcan_test_pucan` is labelled `can`, `pcan` and `unit`. It is byte-level
throughout: build a command and inspect what came out, hand-assemble a record
and see what comes back. It covers the product table, opcode-and-channel
packing, command framing and padding, the slow and fast timing encodings and
their field widths, the option and filter commands, the error-generator
commands and the requests they refuse, the calibration-clear layout, the controller-option bits, firmware-info decoding in
both record forms, receive decoding, several records in one transfer, a
malformed record stopping the walk, status and error records, transmit
encoding, the FD flags and lengths, and the frames `append_tx_frame` refuses.
A green run means the codec does what the file says; it does not exercise a
transfer.
