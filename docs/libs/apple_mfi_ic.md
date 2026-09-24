---
title: apple_mfi_ic
parent: Libraries
---

# apple_mfi_ic

## Overview

A driver for the Apple MFi authentication coprocessor at I2C address `0x11`:
its register map, device and certificate queries, and the challenge-response
signing that both the iAP2 authentication handshake and AirPlay's
`/auth-setup` (MFiSAP) depend on. CarPlay does not proceed without a genuine
chip, so this is the one piece of the stack no test can stand in for.

It is a register driver and nothing more. It does not know what a challenge
means, which is [iap2](iap2.html)'s business through the `MfiSigner`
interface, and it does not own the bus: transactions go through
`libs/i2c_bus`, whose backend follows the host, Linux `/dev/i2c-N` (the SoC's
own controller on a deployed board, or an MCP2221A bound by the in-kernel
`hid_mcp2221` driver on a bench) or the MCP2221A driven over USB HID on macOS.
The split is what makes the retry policy testable: the test substitutes a fake
bus with the coprocessor's measured timing, and a virtual clock for the real
one, and runs the same driver over two transport shapes. OpenSSL is needed for the certificate parsing; when it is
absent the library, and everything above it, is skipped with a warning. The
node that uses it is [carplay](../nodes/carplay.html); the port's design notes
are in [carplay-port](../design/carplay-port.html).

## Public headers

| Header | |
| --- | --- |
| `apple_mfi_ic/apple_mfi_ic.h` | `AppleMFIIC`: the `Register` enum, `DeviceInfo` and `CertificateInfo`, `init`, `read_register`, `read_certificate_data`, `parse_certificate`, `sign_challenge`, `query_device_info`, and a constructor taking an `i2c::Bus`. |

## Using it

Link the CMake target `apple_mfi_ic`; it carries `i2c_bus` and OpenSSL
publicly. The real caller is `iap2::Mcp2221aMfiSigner`, which wraps one
instance behind `MfiSigner` and is what the carplay node constructs. Its
`init` is the whole lifecycle:

```cpp
AppleMFIIC ic;
if (!ic.init(bus_hint)) { return false; }         // opens the bus, wakes the part
const auto info = ic.query_device_info();          // protocol major decides digest size
const auto cert = ic.read_certificate_data();      // PKCS7/DER accessory chain
const auto signature = ic.sign_challenge(digest);  // 128 bytes
ic.close();
```

`bus_hint` names the adapter, a `/dev/i2c-N` path on Linux. Empty falls back
to the `REDLINE_MFI_I2C_DEV` environment variable, then to the bus library's
auto-detection. The node exposes this as `--mfi-i2c-device`.

`apple_mfi_demo` exercises the same sequence against real hardware, printing
the device info, the parsed certificate, and the signature of a fixed
20-byte challenge:

```bash
./build/libs/apple_mfi_ic/apple_mfi_demo [/dev/i2c-N]
```

## Behaviour worth knowing

{: .warning }
Auto-detection prefers an MCP2221A bridge and otherwise takes the first
adapter. On a deployed board that is typically the GPU's DDC bus, where every
probe NACKs and the coprocessor looks dead; on the LattePanda the carrier's
I2C4 header is `/dev/i2c-13`. Set the hint or the environment variable.

The coprocessor sleeps after roughly `30-60 ms` idle. The first START after
that is NACKed, and that NACK is the wake-up: it answers again about
`0.5 ms` later. A single failed transaction therefore means nothing, and a
bus scan that probes each address once walks straight past it. `wake()`
retries a one-byte read for this reason, and `init` fails only after eight
attempts.

A register read is a write of the register address, a STOP, and a separate
read transaction. The part rejects a combined write/read with a repeated
START; on hardware `i2ctransfer w1@0x11 0x00 r1` errors while the same
exchange split in two works.

The retry policy in `read_register` follows timing measured on the
LattePanda's DesignWare controller at `100 kHz` on 2026-09-13. After a
successful register-select write the part is busy for about `1 ms` and NACKs
everything, but the pointer is set; after a NACKed write the pointer is not
set. So a NACKed write is retried after `2 ms` and the pair is redone, while
a read after a good write is retried on its own every `0.5 ms`, without
rewriting the pointer, since re-issuing the write only reopens the busy
window. Before this change the driver retried the pair on every failure and
never read a byte on a native controller, even though `i2cdetect` saw the
part; the MCP2221A's USB round trip had hidden the busy window entirely.

The read retry is bounded by time, `25 ms`, not by a count. The cost of one
failed read depends on the transport: about `0.2 ms` on a native controller,
but around `12 ms` over the hidapi MCP2221A path, where a NACK leaves the
engine latched and it is polled back to idle. A count tuned for one is either
useless or a second-long stall per register on the other. The deadline sits
under the idle-to-sleep threshold, so a read that has not succeeded by then is
not going to, and the outer loop re-selects the register. This also pins one
physical limit: a transport whose NACK recovery is slower than the sleep
threshold can never wake the part, because the recovery itself puts it back
to sleep before the next START.

`sign_challenge` writes the challenge length and data, kicks
`AuthenticationControlAndStatus` with `0x01`, sleeps `400 ms`, then polls the
status every `100 ms` for up to ten attempts until it reads `0x10`, and
finally reads the response length and the response, which the fake and the
hardware both report as 128 bytes. The `10 ms` pause after writing the
challenge data is a fixed wait the source marks as a TODO, not a measured
value.
The expected digest size follows the protocol major version from
`query_device_info`: version 2 signs a 20-byte SHA-1, version 3 a 32-byte
SHA-256.

`AppleMFIIC` is not thread safe. The carplay node shares one instance between
the iAP2 session and the AirPlay receiver's MFiSAP callbacks behind a mutex.

## Tests

```bash
./build/libs/apple_mfi_ic/apple_mfi_ic_test
ctest --test-dir build -R apple_mfi_ic_test
```

`apple_mfi_ic_test` is labelled `apple_mfi_ic` and `unit`. It needs no
hardware and runs on macOS.

It drives `AppleMFIIC` against a fake coprocessor that reproduces the measured
timing (asleep after `30 ms` idle with a wake NACK, `1 ms` busy after a
select, pointer unset by a NACKed write and left alone by a NACKed read,
auto-increment), first with a native-controller transport shape and then with
a bridge shape where every transaction costs `3 ms` and a NACK `12 ms` more.
On each it queries device info cold, lets the part fall asleep and queries
again, and runs a full challenge through to a 128-byte response, then bounds
the time at `3 s` and checks that NACKs were actually exercised. A retry
policy tuned for one transport that stalls on the other fails here.

All of this runs on virtual time. `AppleMFIIC` takes an optional `Clock`,
defaulting to the real one, and the test gives the driver and the fake the
same virtual clock. A wait advances it by exactly what was asked, and a
transaction by its modelled cost. On the real clock a loaded machine
oversleeps. The bridge shape spends 15 ms of the part's 30 ms idle window on
every NACK, so a few milliseconds of scheduler delay put the fake part back
to sleep, and the test failed 16 runs in 112 under load with nothing wrong
in the driver. It now takes a fraction of a second of wall time and gives
the same answer every run.
