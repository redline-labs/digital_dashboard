---
title: xbus
parent: Libraries
---

# xbus

## Overview

The Xsens XBus protocol, and nothing else: the message frame and its checksum,
extended lengths, the two fixed-point formats, the MTData2 item walk with a
`constexpr` parser per data identifier, and the messages this tree sends to a
device. No serial ports, no threads, no zenoh, no allocation in the parse path,
and no spdlog; it reports through `Result<T>` and counters.

The transport half, the serial port, the reader thread and the
Config/Measurement handshake, is [mti610](mti610.html), which depends on this.
Nothing here depends on that. The split is the one `gsof` makes from `bd992`,
for the same reason: a wrong field offset, a fixed-point byte order or a data
identifier's format nibble produces a plausible number rather than a failure,
so this half is made `constexpr` and asserted at compile time. It matters more
here than it did for GSOF because there is no MTi on the bench to disagree.
The node is [mti610_bridge](../nodes/mti610_bridge.html); the traps and the
provenance argument are in the [design notes](../design/mti610.html).

## Public headers

| Header | |
| --- | --- |
| `xbus/byte_order.h` | Big-endian reads and writes, plus `read_fp1632`, `write_fp1632` and the 12.20 form. Not a copy of `gsof/byte_order.h`: sharing would couple the two libraries. |
| `xbus/message.h` | The frame `PREAMBLE(0xFA) | BID | MID | LEN | [EXTLEN:2] | DATA | CHECKSUM`: `parse_message`, `make_message`, `MessageView`, `describe_host_message`. |
| `xbus/message_table.h` | The message ids, transcribed from the SDK's `xsxbusmessageid.h` rather than the LLCP's prose. |
| `xbus/framer.h` | `Framer`: a byte stream into whole, checksum-verified messages, with one-byte resynchronisation. The one piece that is not `constexpr`. |
| `xbus/data_id.h` | The 16-bit data identifier and its group, type and format fields. |
| `xbus/data_table.h` | The one list of MTData2 identifiers, format nibble zeroed. Everything else derives from it. |
| `xbus/mtdata2.h` | `ItemIterator` over `DATA_ID(2) | LEN(1) | PAYLOAD`, one struct per item with a `constexpr parse()`, and `visit_item`. |
| `xbus/commands.h` | `bare(MessageId)`, `encode_output_config`, `parse_output_entry`, `set_option_flags`, `parse_product_code`. No MTi has seen these bytes. |
| `xbus/error.h` | `xbus::Error`: an enum plus two integers, a literal type so parsers can be `constexpr`. |

## Using it

Link the CMake target `xbus`. Push bytes into the framer, take messages out,
and walk the items of each MTData2:

```cpp
xbus::Framer framer;
framer.push(bytes);
while (auto message = framer.next()) {
    if (!message->is(xbus::MessageId::MtData2)) continue;
    xbus::ItemIterator items(message->data);
    while (auto item = items.next()) {
        xbus::visit_item(*item, overloaded {
            [&](const xbus::Acceleration& a) { /* m/s^2, gravity included */ },
            [&](const xbus::PacketCounter& c) { /* the join key */ },
            [&](const auto&) {},
        });
    }
}
```

Adding an identifier is one row in `data_table.h` plus a struct with a
`constexpr parse()` in `mtdata2.h`. Add the row and forget the struct and the
dispatch switch fails to compile.

## Behaviour worth knowing

fp16.32 is not a 48-bit big-endian integer. The value is `round(v * 2^32)` as
an int64, of which the low six bytes are sent in the order
`[b3,b2,b1,b0,b5,b4]`: the fractional part first, then the integer part. A
plain six-byte big-endian read compiles, runs, and returns a plausible wrong
number for every value; 9.81 m/s² reads back as −12451.84. Round-tripping does
not catch this, because an encoder and decoder that share the same wrong byte
order agree perfectly. What catches it is the cross-check in
`tests/test_fixed_point.cpp`: encode one value as float32 and as fp16.32 and
require the decodes to agree. There is no byte order the two can be wrong in
together. Removing the swizzle breaks the build at four `static_assert`s.

A data identifier is not a constant to compare against directly. `0x4020` and
`0x4022` are both acceleration, one in float32 and one in fp16.32; the format
nibble selects the layout and the table keys on the identifier with that
nibble zeroed.

Message ids alias, and only the length tells them apart.
`ReqOutputConfiguration` and `SetOutputConfiguration` are both `0xC0`;
`ReqBaudrate` and `SetBaudrate` are both `0x18`, and around thirty such pairs
exist. `describe_host_message(id, hasPayload)` is what turns an id into
something a human should read.

`0xFA` appears inside payloads. There is no escaping and no trailer, so a
preamble byte inside an accelerometer reading is ordinary. The framer resyncs
by dropping exactly one byte and revalidating, never by scanning ahead to the
next preamble, because scanning would skip real messages. A corollary that
looks like a bug the first time: a false preamble claiming a long payload
makes the framer wait rather than guess, and everything behind it arrives at
once when the candidate fails its checksum.

A `LEN` of `0xFF` means the two bytes that follow carry a 16-bit length. In
practice only a large MTData2 uses it, and 15,321 of the 15,325 messages in
the vendor's sample logs do.

`SetOutputConfiguration` replaces the whole list. XBus has no way to change
one output, so a caller that wants to leave an entry alone must re-send it.
This is the opposite of how the BD992's APPFILE works, and it is what
`mti610`'s `plan_writes` handles.

`PortConfig` words are read and round-tripped opaquely and never composed:
the LLCP documents `SetPortConfig`'s layout only as an unlabelled figure.

## Tests

```bash
ctest --test-dir build -L xbus --output-on-failure   # xbus_test_framing, xbus_test_fixed_point, xbus_test_mtdata2, xbus_test_commands
```

All four are labelled `xbus` and `unit`, and all are compile-time heavy. The
authority comes from two independent sources: the LLCP (MT0101P rev 2019.C)
for prose and payload layouts, and the SDK's own headers
(`xsxbusmessageid.h`, `xsdataidentifier.h`, `xsmessage.h`) for every numeric
constant, used as a constants oracle only.

`tests/golden/golden_messages.h` keeps two kinds of vector apart and labels
every one. VENDOR vectors are five complete messages Xsens printed with their
checksums, plus two lifted from the SDK's `.mtb` logs that settle the
`FirmwareRev` layout and the output-configuration entry layout;
`make_message()` reproduces all of them byte for byte, which is what the
framing of every command rests on. Two of the synthetic checksums in the
first draft of that file were wrong, and the vendor vectors are what caught
it. SYNTHETIC vectors cover extended length, a preamble byte in a payload, an
MTData2 body, a truncated header. They check that the parser is
self-consistent and refuses malformed input; they cannot confirm the wire
format. `tests/golden/README.md` has the full argument.

The SDK ships two multi-megabyte `.mtb` logs, which are raw XBus streams. Both
frame end to end with every byte accounted for, 15,325 messages, and the C++
framer fed in 37-byte reads and an independent Python framer agree exactly.
That is what extended-length framing rests on. The logs are Xsens-licensed
and not checked in; `tests/golden/tools/verify_sdk_corpus.py` re-runs the
check.

The command encodings in `commands.h` are checked against the LLCP's tables
and against their own decoders, not against a device. No MTi has seen these
bytes. It is the same caveat `libs/gsof`'s command test carries, and that
library earned it.
