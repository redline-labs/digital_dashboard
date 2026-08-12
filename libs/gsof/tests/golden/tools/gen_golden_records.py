#!/usr/bin/env python3
"""Emit libs/gsof/tests/golden/golden_records.h from the captured pcaps."""
import sys, os, glob, importlib.util

spec = importlib.util.spec_from_file_location("extract", os.path.join(os.path.dirname(__file__), "extract.py"))
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)

PCAPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "pcaps")

# Which capture to take each record from, and the C++ name to give it.
WANT = [
    (1,  "applus60_gsof1.pcap",  "kPositionTime"),
    (2,  "applus60_gsof2.pcap",  "kLatLongHeight"),
    (3,  "applus60_gsof3.pcap",  "kEcefPosition"),
    (6,  "applus60_gsof6.pcap",  "kEcefDelta"),
    (7,  "applus60_gsof7.pcap",  "kTangentPlaneDelta"),
    (8,  "applus60_gsof8.pcap",  "kVelocity"),
    (9,  "applus60_gsof9.pcap",  "kDopInfo"),
    (10, "applus60_gsof10.pcap", "kClockInfo"),
    (11, "applus60_gsof11.pcap", "kPositionVcv"),
    (12, "applus60_gsof12.pcap", "kPositionSigma"),
    (15, "applus60_gsof15.pcap", "kReceiverSerial"),
    (16, "applus60_gsof16.pcap", "kCurrentTimeUtc"),
    (27, "applus60_gsof27.pcap", "kAttitudeInfo"),
    (33, "applus60_gsof33.pcap", "kAllSvBrief"),
    (34, "applus60_gsof34.pcap", "kAllSvDetailed"),
    (35, "applus60_gsof35.pcap", "kReceivedBase"),
    (37, "applus60_gsof37.pcap", "kBatteryMemory"),
    (38, "applus60_gsof38.pcap", "kPositionType"),
    (40, "applus60_gsof40.pcap", "kLbandStatus"),
    (41, "applus60_gsof41.pcap", "kBasePosition"),
    (49, "apx_18_fullnavinfo_single.pcap", "kInsFullNav"),
    (50, "apx_18_fullrmsinfo_single.pcap", "kInsRms"),
]


def stream_of(name):
    linktype, packets = extract.read_pcap(os.path.join(PCAPS, name))
    return b"".join(p for p in (extract.payload_of(linktype, pk) for pk in packets) if p)


def first_record(name, want_type):
    pages = {}
    for status, ptype, data in extract.dcol_packets(stream_of(name)):
        if ptype != 0x40:
            continue
        tx, pg, mx = data[0], data[1], data[2]
        pages.setdefault(tx, bytearray())
        pages[tx] += data[3:]
        if pg >= mx:
            for rec in extract.records(bytes(pages[tx])):
                if rec[0] == want_type:
                    return rec[2]
            pages.pop(tx)
    raise SystemExit(f"{name}: no record {want_type}")


def arr(name, body):
    hexes = ", ".join(f"0x{b:02X}" for b in body)
    wrapped, line = [], "    "
    for tok in hexes.split(", "):
        if len(line) + len(tok) + 2 > 96:
            wrapped.append(line.rstrip())
            line = "    "
        line += tok + ", "
    wrapped.append(line.rstrip().rstrip(","))
    joined = "\n".join(wrapped)
    return f"inline constexpr std::array<std::uint8_t, {len(body)}> {name} {{\n{joined}\n}};\n"


HEADER = '''// SPDX-License-Identifier: GPL-3.0-or-later
//
// GSOF record bodies captured from real Trimble receivers.
//
// GENERATED -- do not edit. Regenerate with the script recorded in README.md.
//
// These are the record BODIES: the TYPE and LENGTH bytes are stripped, because
// that is what Record::parse() is handed. Each is the first record of its type
// in the corresponding capture.
//
// Provenance and licence: extracted from the test captures in
// https://github.com/trimble-oss/trimble_driver_ros, which is
//
//   Copyright (c) 2024 Trimble Inc., BSD 2-Clause.
//
// Redistribution in binary form must reproduce that copyright notice, this
// list of conditions and the following disclaimer in the documentation and/or
// other materials provided with the distribution. THIS SOFTWARE IS PROVIDED BY
// THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES ARE DISCLAIMED.
//
// WHY REAL CAPTURES RATHER THAN HAND-WRITTEN BYTES: a vector authored from the
// same reading of the ICD as the parser agrees with the parser by
// construction, including where both are wrong. These came off receivers, and
// they cross-check each other -- records 35 and 41 report the same base
// station, and record 7's tangent-plane baseline is the distance between
// record 2's rover and that base. No amount of reading the ICD produces that.

#ifndef GSOF_GOLDEN_RECORDS_H
#define GSOF_GOLDEN_RECORDS_H

#include <array>
#include <cstdint>

namespace gsof::golden
{

'''

FOOTER = '''
} // namespace gsof::golden

#endif // GSOF_GOLDEN_RECORDS_H
'''


def main():
    out = [HEADER]
    for rtype, pcap, name in WANT:
        body = first_record(pcap, rtype)
        out.append(f"// GSOF {rtype}, from {pcap} ({len(body)} bytes)\n")
        out.append(arr(name, body))
        out.append("\n")
    out.append(FOOTER.lstrip("\n"))
    dest = sys.argv[1]
    with open(dest, "w") as f:
        f.write("".join(out))
    print(f"wrote {dest}")


if __name__ == "__main__":
    main()
