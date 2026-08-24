#!/usr/bin/env python3
"""Emit libs/gsof/tests/golden/golden_bd992.h from a raw BD992 GSOF stream.

Unlike gen_golden_records.py, which lifts one record body per capture, this
emits WHOLE TRANSMISSION PAYLOADS -- every record the receiver sent in one
instant, in the order it sent them. That is what makes the cross-record
assertions in test_records.cpp possible: records 2, 3, 62 and 70 describing one
position four ways can only be checked against each other if they came out of
the same epoch.

Usage: gen_golden_bd992.py <raw-stream-file> [provenance] > golden_bd992.h

A GSOF CAPTURE IS A POSITION FIX. Not only through the position records: the
satellite azimuths and elevations in records 33, 34 and 48, against the
timestamp in record 1, pin the observer down on their own, so removing the
position records does not make a capture safe to publish. Take the capture
somewhere you are content to have in the repository.

The raw stream is what `bd992_bridge --dump-gsof` writes, or equivalently a
plain read of the receiver's GSOF TCP socket.
"""
import sys


def dcol_packets(stream):
    """Yield (status, type, data) for each well-formed DCOL packet."""
    i = 0
    while i < len(stream):
        if stream[i] != 0x02:
            i += 1
            continue
        if i + 6 > len(stream):
            break
        status, ptype, length = stream[i + 1], stream[i + 2], stream[i + 3]
        total = length + 6
        if i + total > len(stream):
            break
        if stream[i + total - 1] != 0x03:
            i += 1
            continue
        data = stream[i + 4:i + 4 + length]
        if stream[i + total - 2] != (status + ptype + length + sum(data)) & 0xFF:
            i += 1
            continue
        yield status, ptype, data
        i += total


def transmissions(stream):
    """Yield reassembled GENOUT payloads, transport header stripped."""
    held, expect, txn = bytearray(), 0, None
    for _, ptype, data in dcol_packets(stream):
        if ptype != 0x40:
            continue
        tx, page, last = data[0], data[1], data[2]
        if page == 0:
            held, expect, txn = bytearray(data[3:]), 1, tx
        elif txn == tx and page == expect:
            held += data[3:]
            expect += 1
        else:
            held, txn = bytearray(), None
            continue
        if page == last:
            yield bytes(held)
            held, txn = bytearray(), None


def record_types(payload):
    out, at = [], 0
    while at + 2 <= len(payload):
        rtype, length = payload[at], payload[at + 1]
        if at + 2 + length > len(payload):
            break
        out.append(rtype)
        at += 2 + length
    return out


def arr(name, body):
    hexes = [f"0x{b:02X}" for b in body]
    wrapped, line = [], "    "
    for tok in hexes:
        if len(line) + len(tok) + 2 > 96:
            wrapped.append(line.rstrip())
            line = "    "
        line += tok + ", "
    wrapped.append(line.rstrip().rstrip(","))
    joined = "\n".join(wrapped)
    return f"inline constexpr std::array<std::uint8_t, {len(body)}> {name} {{\n{joined}\n}};\n"


HEADER = '''// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two whole GSOF transmissions, read off a live Trimble BD992.
//
// GENERATED -- do not edit. Regenerate with the script recorded in README.md.
//
// These are TRANSMISSION PAYLOADS, not record bodies: the DCOL framing and the
// three-byte transport header are stripped, and what is left is the
// back-to-back TYPE | LENGTH | BODY records the receiver sent in one instant.
// golden_records.h holds one record body per type; this holds every record of
// one epoch, in order, which is a different and stronger thing to test against.
//
// WHY A WHOLE EPOCH. A per-record vector can only be checked against the ICD --
// the same document the parser was written from, so agreement proves nothing
// about either. A whole epoch can be checked against ITSELF: records 2, 3, 62
// and 70 describe one position in four coordinate systems, record 28's RTK age
// is record 38's correction age, record 48's pages carry record 34's
// satellites and the ones it had to truncate, and records 1, 62 and 91 stamp
// the same instant. Those agreements cannot survive a transposed field, and no
// amount of re-reading the specification can manufacture them.
//
// Provenance: our own BD992, %(when)s. No third-party licence attaches.
//
// A GSOF capture is a position fix -- through the position records, and through
// the satellite azimuths and elevations against the timestamp -- so this was
// taken somewhere publishable rather than wherever the receiver happened to be.

#ifndef GSOF_GOLDEN_BD992_H
#define GSOF_GOLDEN_BD992_H

#include <array>
#include <cstdint>

namespace gsof::golden::bd992
{

'''

FOOTER = '''
} // namespace gsof::golden::bd992

#endif // GSOF_GOLDEN_BD992_H
'''


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    when = sys.argv[2] if len(sys.argv) > 2 else "captured live"
    stream = open(sys.argv[1], "rb").read()

    seen = []
    for payload in transmissions(stream):
        seen.append(payload)
        if len(seen) == 2:
            break
    if len(seen) < 2:
        raise SystemExit("need at least two transmissions in the capture")

    out = [HEADER % {"when": when}]
    for payload, name in zip(seen, ("kEpochMain", "kEpochCodePosition")):
        types = record_types(payload)
        out.append(f"// {len(payload)} bytes, {len(types)} records: "
                   + ", ".join(str(t) for t in types) + "\n")
        out.append(arr(name, payload))
        out.append("\n")
    out.append(FOOTER.lstrip("\n"))
    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
