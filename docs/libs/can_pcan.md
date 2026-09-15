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

The X6 is six channels behind three USB interfaces of two, so channel 4 is
interface 2's local channel 0. That mapping is the part most likely to need
adjusting against real hardware; no X6 has been tried.

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

Opcode `0x00d` is reserved and the barrier is `0x010`; a command the device
does not implement is ignored in silence. Commands are eight bytes each,
several packed into one buffer padded to 512 bytes (64 on a low-speed link)
and terminated by an end-of-collection marker. The "driver loaded" vendor
request is what takes the adapter out of its idle state; without it nothing
answers on the bulk endpoints. Calibration messages are the device's own
timestamp traffic and are switched off, or they arrive as a stream of records
nothing wants. `readTimeoutMs` is how long a bulk read waits before the reader
thread checks whether it should stop, not a bus timeout; a quiet bus produces
no transfers at all.

## Tests

```bash
ctest --test-dir build -L pcan       # can_pcan_test_pucan
```

`can_pcan_test_pucan` is labelled `can`, `pcan` and `unit`. It is byte-level
throughout: build a command and inspect what came out, hand-assemble a record
and see what comes back. It covers the product table, opcode-and-channel
packing, command framing and padding, the slow and fast timing encodings and
their field widths, the option and filter commands, firmware-info decoding in
both record forms, receive decoding, several records in one transfer, a
malformed record stopping the walk, status and error records, transmit
encoding, the FD flags and lengths, and the frames `append_tx_frame` refuses.
A green run means the codec does what the file says; it does not exercise a
transfer.
