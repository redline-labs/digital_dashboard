---
title: canopen
parent: Libraries
---

# canopen

## Overview

CANopen for the devices on this bus: an EDS parser and validator, an SDO
client, an NMT master, an LSS master, PDO mapping read out of the object
dictionary, a virtual bus with its own clock, a stub device built from an EDS,
and a code generator (`canopen_code_gen`, driven by the `generate_eds_code()`
CMake macro) that turns an EDS into typed PDO helpers. Every service is
written against one small `Bus` interface, and time is part of that interface
rather than read from the system clock, which is the whole reason the
reconfiguration sequence can be tested on a laptop.

The zenoh transport is a separate target, `canopen_zenoh`, because it pulls in
zenoh, capnp and the schemas and nothing that only wants to parse an EDS or
drive a stub should carry that; `CANOPEN_WITH_ZENOH` turns it off. This is not
a complete CANopen stack: the stub implements what reconfiguration touches,
expedited and segmented SDO upload, expedited download, NMT, LSS, heartbeat
and boot-up, and PDO production is caller-driven. The node is
[grayhill_keypad](../nodes/grayhill_keypad.html); frames reach a real adapter
through [can_bridge](../nodes/can_bridge.html).

## Public headers

| Header | |
| --- | --- |
| `canopen/bus.h` | `Bus`: `send`, `poll`, `now`, `subscribe`; `wait_until`. Handlers are called in order and a frame is never consumed. |
| `canopen/eds_ast.h` | `ObjectDictionary`, `Object`, `SubObject`, `DataType`, `AccessType`, `NodeIdExpr`, `DeviceInfo` with the LSS flag and bit-rate table. |
| `canopen/eds_parser.h` | `parse_eds` (never throws, never returns nothing), `validate`, `Diagnostic`. |
| `canopen/eds_grammar.h` | The lexy grammar: bracketed names and raw lines, nothing more. |
| `canopen/sdo.h` | `SdoClient`: typed upload and download, `store_parameters`, `restore_parameters`; `SdoData` with the served width; `SdoAbortCode`. |
| `canopen/nmt.h` | `NmtMaster`: `command`, `state`, `reset_and_wait`, `wait_for_bootup`, emergency and state callbacks. |
| `canopen/lss.h` | `LssMaster` behind a `singleNodeBus` assertion; `LssBitrate` and the CiA 301 table. |
| `canopen/pdo_mapping.h` | `read_pdo_mapping`: what a PDO carries, from `0x1600`/`0x1A00`, not assumed. |
| `canopen/pdo_bits.h` | `get_bits`, `set_bits`, `sign_extend`: the little-endian packing rule, in one place. |
| `canopen/virtual_bus.h` | `VirtualBus`: virtual clock, scheduled frames, a bit rate, `sent()` for asserting on the wire. |
| `canopen/stub_device.h` | `StubDevice`: a device made of its own EDS. |
| `canopen/zenoh_bus.h` | `ZenohBus`: frames out on `vehicle/can0/tx`, in on `vehicle/can0/rx`, delivered inside `poll()`. |

## Using it

Link `canopen`, plus `canopen_zenoh` for `ZenohBus`, plus the generated
library for typed PDOs: `generate_eds_code(canopen_grayhill DS401_3K_C.eds)`
in `eds/grayhill/` produces `canopen_grayhill_helpers.h` and
`canopen_grayhill_node.h`. The keypad node wires them like this:

```cpp
canopen::ZenohBus bus(config.txKey, config.rxKey);
canopen::NmtMaster nmt(bus);
canopen::SdoClient sdo(bus, config.nodeId);
canopen_grayhill::node device(config.nodeId);

nmt.command(canopen::NmtCommand::EnterPreOperational, config.nodeId);
auto result = sdo.download_u16(0x1017, 0, heartbeatMs);
nmt.command(canopen::NmtCommand::Start, config.nodeId);
for (;;) {
    bus.poll(canopen::Duration { 20 });   // handlers run here, on this thread
}
```

The reconfiguration tool swaps in a `VirtualBus` with a `StubDevice` attached
and runs the same code.

## Behaviour worth knowing

EDS section names are hexadecimal and the values inside them are decimal
unless prefixed. `[1018]` is index 0x1018 and `[1A00sub3]` is 0x1A00 sub 3,
but `DefaultValue=255` is 255 and `DefaultValue=0xFF` is 255 too. Mixing the
two rules up produces a dictionary in which nothing is where you asked for it,
and the previous test shared the wrong assumption with the parser and passed.
CiA 306 has no inheritance: an EDS that omits an object is asserting the device
does not implement it, and there is no DS301 or DS401 baseline to fall back
on. COB-ID defaults are written as `$NODEID+0x40000180`, and a parser that
cannot represent that expression can only fall back to profile constants and
look like it succeeded.

Parsing and validation are separate calls. `parse_eds` answers "what does this
file say"; `validate` answers "is what it says coherent", comparing sub 0
against the real sub count, declared objects against present ones, PDO
mappings against the 64-bit limit, and defaults against their own limits. The
grammar recognises only "a bracketed name followed by lines", so a section it
cannot classify has nothing to fall through to; the previous grammar dropped
`[1A00]` because `A` is not a decimal digit, and TPDO1's mapping vanished with
it. The generator runs with `--strict` for the same reason: code that fell
back to constants would build clean with wrong numbers in it.

Every SDO exchange is confirmed. The node this replaced sent a heartbeat write
and never looked at the answer, so a device that aborted looked exactly like
one that accepted. An upload reports the width the server actually served,
because MoTeC's PDM Manager compares the response command byte for exact
equality, `0x4B` for the two-byte `0x2010:02` and `0x4F` for the one-byte
`0x1800:02`, and a width mismatch is a different error from a value mismatch.
Segmented transfer exists only for the `VISIBLE_STRING` objects. The frames an
exchange sends are exposed so a dry run prints what an apply would send, from
the same code.

{: .warning }
LSS switch-state-global is a broadcast: every LSS-capable device on the bus
enters configuration mode and the node ID written lands on all of them.
Nothing here sends it unless the caller has asserted `singleNodeBus`, and
`Refused` is the answer otherwise.

LSS goes last in any sequence. Node ID and bit rate persist through LSS Store,
a different mechanism from the SDO `0x1010` save, and take effect on the next
reset; change your own addressing before you are finished talking and you lose
the device, with nothing to recover it but a bit-rate sweep. The Grayhill
keypad ships at 250 kbit/s and MoTeC ships keypads at 1 Mbit/s, which is the
single most common reason a keypad "cannot be found". The keypad's EDS
declares 10 kbit/s unsupported.

Heartbeat state `0x00` is the boot-up message, seen once per reset;
`NmtMaster` counts boot-ups per node so `reset_and_wait` can wait for that
frame rather than sleep a guessed interval. PDOs pack little-endian, first
mapped entry in the least significant bits of byte 0, and `get_bits` reads
zero beyond the buffer, so a short frame yields zeroed fields instead of
adjacent stack. `ZenohBus` hands received frames over inside `poll()` on the
caller's thread, bounded at 4096 with a `dropped()` counter, and is still
untested against hardware; the stub transport is the one with tests behind it.

## Tests

```bash
ctest --test-dir build -L canopen    # canopen_test_eds_parse, canopen_test_runtime, canopen_test_generated_grayhill
```

`canopen_test_eds_parse` and `canopen_test_runtime` are labelled `canopen` and
`unit`, as is `canopen_test_generated_grayhill` under `eds/grayhill/`. The
shipped EDS is part of what is under test: a file this repo generates code
from has to parse and validate clean, with its hexadecimal indices spelled out.

The virtual bus and the stub are what let the runtime test run the whole
reconfiguration with no hardware and no wall clock. Time on a `VirtualBus`
moves only in `poll()`, so a one-second SDO timeout costs a few loop
iterations and a wait for a boot-up either sees it or does not. The bus
carries a bit rate and drops a frame injected at a different one; after an LSS
reconfiguration the boot-up goes out at the new rate, so a tool that forgets
to switch its own interface fails here rather than on a bench. `StubDevice` is
seeded from the EDS's defaults with `$NODEID` resolved, so writes land and
reading back is a real test; response widths come from the declared
`DataType`; a read-only object or an out-of-range value returns a real abort;
non-volatile memory is separate from live values, so nothing survives a reset
until something performs a Store; and node ID and bit rate change only on
reset, after which the device answers only at the new address and rate.
`make_read_only` models firmware stricter than its own EDS, and
`set_present(false)` is the "keypad not found" case.

The runtime test covers upload widths, identity, segmented upload, the three
abort cases, missing objects, timeout, the exchange log, reset and boot-up,
state tracking, the PDM Manager compatibility sequence, unsaved changes not
surviving, the two-step COB-ID write, and LSS refused without the assertion,
ignored outside configuration mode, run in full, and refusing a bad bit rate
or node ID. The parser test covers the real EDS and its PDO mappings, a
survivable malformed line, unknown access and data types, signed limits, a
sub-section before its parent, the three `validate` catches, and a file that
is not an EDS at all.
