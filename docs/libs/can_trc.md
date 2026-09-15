---
title: can_trc
parent: Libraries
---

# can_trc

## Overview

PCAN `.trc` traces, read and written, and a trace as a replay channel. It sits
above [can](can.html) rather than beside it: the reader needs
`can::dlc_to_length` to turn a CAN FD length code into a byte count, and the
replay backend implements `can::Backend`. It deliberately does not depend on
[can_backends](can_backends.html), because the registry is what pulls the
backends together and a dependency the other way would be a cycle.

What it is not is the way to record a trace. A `Channel::send()` only ever sees
the frames this process transmits, and a trace worth keeping has both
directions of a bus in it, so recording lives as a tap inside
[can_bridge](../nodes/can_bridge.html) (`nodes/can_bridge/trc_recorder.cpp`)
and uses the `Writer` here.

## Public headers

| Header | |
| --- | --- |
| `can_trc/trc.h` | `Version`, `Columns`, `RecordKind`, `Record`, `FileHeader`, `ReadStats`; the streaming `Reader`; `BusInfo`, `WriterOptions` and `Writer`; the OLE date conversions. |
| `can_trc/trc_backend.h` | `ReplayOptions` (`paced`, `speed`, `loop`) and `make_trc_backend`. |

## Using it

Link the CMake target `can_trc`. As a channel, `trc:<path>` replays the whole
file and `trc:<path>/<bus>` narrows it to one bus of a multi-bus trace, so one
file with three buses in it becomes three `can::Channel`s and nothing above the
registry has to learn anything. Replay options are backend-wide, set through
`DefaultRegistryOptions::trc`. Writing is what the bridge's recorder does:

```cpp
can::trc::WriterOptions options;
options.buses.push_back(busInfo);                 // name, connection, bit rate
auto writer = can::trc::Writer::create(path, options);

can::trc::Record record;
record.kind = can::trc::RecordKind::Data;
record.offsetUs = elapsedUs;
record.bus = 1;
record.isTx = false;
record.frame = frame;
(*writer)->write(record);
```

## Behaviour worth knowing

There are seven versions of the format and they are not variations on a theme:
the column order changes between them, v2.x lets the file declare its own
layout in a `;$COLUMNS=` line, and which columns a line even has depends on the
record kind. This build reads 1.0 through 2.1 and writes 2.1. A 3.0 file is
read for everything it shares with 2.1; its CAN XL records are counted in
`ReadStats::unsupported` and skipped, because `helpers::CanFrame` has no VCID,
SDT or AF field and widening it would reach every CAN consumer in the tree for
a bus none of them talk to.

{: .warning }
The width of the ID token is what says whether the identifier is 11-bit or
29-bit. `0123` and `00000123` are different messages on a real bus, and the
only thing distinguishing them in the file is four characters against eight.
Parse the token, not the number.

A column headed `l` is a byte count and one headed `L` is a CAN FD length
code, where 15 means 64 bytes. Reading one as the other gives a frame of the
wrong length with nothing to indicate it. The writer emits
`N,O,T,B,I,d,R,l,D`, with `l`, so no reader has to own the FD length table to
get the payload length right.

A malformed line is a malformed line, not a malformed file: it is counted in
`ReadStats::badLines` and skipped. The previous implementation failed the
whole file, so one bad line in a 24,000-line trace returned nothing at all.
`parse_version` and `parse_columns` do fail on anything they do not recognise,
because guessing a version picks a column layout and produces plausible wrong
frames.

`Record::offsetUs` is a 64-bit integer on purpose. `mock_data/data/pdm32_log.trc`
runs from 4294967270.343 ms to 4295008779.456 ms, crossing 2^32 milliseconds
inside the file, so a 32-bit accumulator wraps and a float has seven
significant digits for a value that needs thirteen. `$STARTTIME` is an OLE
Automation date, days since 1899-12-30 with the time of day as the fraction;
only v1.0 files have none, and they are the only case where a record's
absolute time is unknowable.

The `Reader` streams one record at a time, since a trace is the one CAN
artefact that is routinely hundreds of megabytes. The replay backend paces
frames at their recorded intervals unless `paced` is off, which is what a test
or an import wants; `stop()` does not wait out the gap to the next frame,
which is the difference between a node that shuts down and one that appears
to hang. `send()` on a replay channel drops and counts, the same as
transmitting onto a bus with nothing else attached. The path lands in
`ChannelId::device` unescaped because a trailing `/N` is only read as a channel
when the whole segment is digits; the one path this cannot express is a file
whose name is a bare number.

## Tests

```bash
ctest --test-dir build -L can_trc    # can_trc_test_read, can_trc_test_write, can_trc_test_backend
```

All three are labelled `can_trc` and `unit`. `can_trc_test_read` uses PEAK's
own example blocks from the format specification as the fixture for each
version, so what is pinned is the format rather than this implementation's
idea of it; every one of v1.0 through v1.3 and v2.1 failed the previous
parser. The rest of that file is malformed and awkward input, and the
load-bearing case is `bad_line_in_the_middle`. `can_trc_test_write` writes and
reads back every field the old parser threw away, and reads the two real
traces under `mock_data/data/` asserting their exact record counts and that
nothing was rejected; nothing in the tree read those files before, which is
why a parser that returned zero frames went unnoticed. `can_trc_test_backend`
pins that a file behaves like a bus: every bus is read, the bus filter works,
pacing and unpaced replay behave, `stop()` interrupts a paced wait, looping
keeps time moving forwards, a missing file fails at open, and `enumerate()` is
empty.
