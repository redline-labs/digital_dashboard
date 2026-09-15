---
title: gsof
parent: Libraries
---

# gsof

## Overview

The Trimble GSOF protocol, and nothing else: the DCOL packet frame, the
multi-page transmission, the record walk, one struct per record with a
`constexpr` parse, and the APPFILE command encodings. No sockets, no threads,
no zenoh, no allocation in the parse path, and no spdlog; it reports through
`Result<T>` and counters, and the decision about what is worth a log line
belongs to the node.

The transport half, TCP, reconnection and the command/response exchange, is
[bd992](bd992.html), which depends on this. Nothing here depends on that. The
split is the one `can` makes from `can_pcan`: bytes on one side, transports on
the other. It exists so that this half can be `constexpr`, which is what turns
a wrong field offset into a build error rather than a plausible latitude; the
argument is in the [design notes](../design/bd992.html). The node that uses
both is [bd992_bridge](../nodes/bd992_bridge.html).

## Public headers

| Header | |
| --- | --- |
| `gsof/byte_order.h` | Big-endian scalar reads and writes over a span, assembled byte by byte so they are usable in constant evaluation. |
| `gsof/trimcomm.h` | The DCOL packet: `parse_packet`, `encode_packet`, `make_packet`, the checksum rule, packet types. |
| `gsof/framer.h` | `Framer`: a byte stream into whole, checksum-verified packets, with resynchronisation. The one piece that is not `constexpr`. |
| `gsof/transport.h` | `PageAssembler`: the `TX_NUM | PAGE_IDX | MAX_PAGE_IDX` reassembly shared by GENOUT and APPFILE. |
| `gsof/tlv.h` | The `TYPE | LENGTH | BODY` walk that GSOF records and application-file records both use. |
| `gsof/record_table.h` | The one list of GSOF records: wire id, struct name, snake-case name. Everything else derives from it. |
| `gsof/records.h` | One struct per record with a `constexpr parse()`; field names carry wire units. |
| `gsof/record_iterator.h` | `visit_record`: dispatch from a raw record to its parsed struct, as a visitor rather than a variant. |
| `gsof/commands.h` | APPFILE and GETAPPFILE: `OutputMessage`, `Frequency`, `get_application_file`, `get_options`, `encode_application_file`, `parse_application_file`. |
| `gsof/error.h` | `gsof::Error`: an enum plus two integers, a literal type so parsers can be `constexpr`. |

## Using it

Link the CMake target `gsof`. The pipeline is bytes to `Framer` to
`PageAssembler` to `TlvIterator` to `visit_record`, and each stage takes what
the previous one produced:

```cpp
gsof::Framer framer;
gsof::PageAssembler pages;

framer.push(bytes);
while (auto packet = framer.next()) {
    auto fed = pages.feed(packet->data);
    if (!fed || *fed != gsof::PageAssembler::Feed::Complete) continue;

    gsof::TlvIterator records(pages.payload());
    while (auto raw = records.next()) {
        gsof::visit_record(*raw, overloaded {
            [&](const gsof::LatLongHeight& r) { /* radians on the wire */ },
            [&](const auto&) {},
        });
    }
}
```

Adding a record type is one row in `record_table.h` plus a struct with a
`constexpr parse()` in `records.h`. Add the row and forget the struct and the
dispatch switch fails to compile.

## Behaviour worth knowing

The packet is `STX(0x02) | STATUS | TYPE | LENGTH | DATA | CHECKSUM |
ETX(0x03)`, where `CHECKSUM = (STATUS + TYPE + LENGTH + sum(DATA)) mod 256`
covers everything between STX and the checksum and neither frame marker.
Big-endian throughout. The DATA of both GENOUT (`0x40`, the report stream) and
APPFILE (`0x64`, configuration in both directions) begins with `TX_NUM |
PAGE_IDX | MAX_PAGE_IDX`, so one page assembler serves both, and inside a
reassembled payload records are `TYPE | LENGTH | BODY` back to back, which is
why the walk lives once in `tlv.h`.

The framer resynchronises by design. The reference ROS driver's equivalent
leaves its buffer untouched on a checksum mismatch, so one corrupted byte
wedges the stream permanently; this one drops the bad candidate and carries
on.

A record can straddle a page boundary, so pages are concatenated before any
record header is read. Parsing per page works on every small record and
corrupts exactly the large ones.

Variable-length records are distinguished by length alone: record 8 is 13 or
17 bytes, record 27 is 42 or 70, and there is no flag. Record 1 is the only
record with time-of-week before the week number. Record 70's geoid model name
has no length prefix and no terminator: it runs from byte 26 to the end, so
its length is the record length minus 24.

Record 48 pages are not transport pages. The transport paging is reassembled
before any record is read; record 48 has its own, inside the record, and
several complete, separately framed GSOF 48 records arrive in one
transmission, each carrying "page N of M". A consumer joins them itself, and a
record 48 entry is a signal group, not a satellite: the same PRN can appear on
more than one page with the same elevation and azimuth but a different SNR
triple. Joining the pages into a PRN-keyed map silently drops entries.

Records longer than the ICD says are accepted and their tails ignored. Trimble
extends records in place, and a parser that refused would turn a firmware
update into an outage.

Field names carry the wire's units, not the ones a consumer wants: radians in
records 2, 27 and 41, degrees in record 49. The conversion to degrees happens
once, in the node, so the structs still describe the bytes.

The APPFILE frequency byte is not an ordinal: 10 Hz is `0x01` and 1 Hz is
`0x03`, which is why `Frequency` is an enum with names rather than a number.

## Tests

```bash
ctest --test-dir build -L gsof      # gsof_test_framing, gsof_test_pages, gsof_test_records, gsof_test_commands
```

All four are labelled `gsof` and `unit`. Most of `test_records.cpp` runs at
compile time: every parser is `constexpr`, and a golden record plus a
`static_assert` means that if it builds, the offsets are right.

The goldens are captures from real receivers, not hand-written bytes, because
a vector authored from the same reading of the ICD as the parser agrees with
the parser by construction, including where both are wrong. Captured records
cross-check each other in ways no reading of the specification can: records
35 and 41 report the same base station bit for bit, record 7's tangent-plane
baseline is the distance between record 2's rover and that base, and record
3's ECEF vector resolves to record 2's latitude and longitude.
`libs/gsof/tests/golden/README.md` has the provenance and how to regenerate.

The records added for a BD992 (13, 14, 28, 48, 62, 70, 74, 91, 92 and 96) were
validated against a live receiver, but that capture was taken privately and is
not in the repository, because a GSOF capture is a position fix. What stands
in for it in `test_records.cpp` is a set of synthetic vectors, labelled as
such, covering the parsers' arithmetic: the nested variable lengths in record
91, the page nibbles in 48, the model name in 70, the strides in 13 and 14 and
the epoch count in 74. Those catch a logic error; they cannot catch a field
offset that is wrong the same way in both parser and vector. The README has
the procedure for taking a capture somewhere publishable.

The command encodings in `commands.h` are checked against the ICD's tables and
against their own decoders, not against a receiver: as of 2026-08-23 the only
socket tried was configured output-only. If a BD992 ignores a configuration
command, the constants in `commands.h` are the first thing to suspect, and
`bd992_bridge --probe` is the tool. The record parsers no longer share this
caveat.
