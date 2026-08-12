# Golden GSOF records

`golden_records.h` is generated. It holds the body of one record of each type
this library parses, taken from packet captures of real Trimble receivers.

## Why captures and not hand-written bytes

A byte vector authored from the same reading of the ICD as the parser agrees
with the parser by construction — including everywhere both are wrong. These
came off receivers, and they cross-check each other in ways no amount of
reading the specification can:

- Records 35 (Received Base) and 41 (Base Position) report the same base
  station, to the bit, from different captures.
- Record 7's tangent-plane baseline (16 051 m north, 1 457 m west) is the
  distance between record 2's rover position and that base.
- Record 3's ECEF vector has the magnitude of an Earth radius and resolves to
  the same latitude and longitude as record 2.

`tests/test_records.cpp` asserts all three. A transposed field or a flipped
endianness breaks them immediately, which a self-consistent hand-written vector
would not.

## Provenance and licence

Extracted from the test data of <https://github.com/trimble-oss/trimble_driver_ros>:

> Copyright (c) 2024 Trimble Inc.
> BSD 2-Clause licence.

The BSD 2-Clause terms are reproduced in the generated header, which is where
they need to live for a binary distribution. The captures are of an Applanix
APX-60/APX-18 and an SPS986 rather than a BD992 — the record layouts are common
to the receiver family, and the two INS records (49, 50) are the ones a BD992
does not produce at all.

## Regenerating

The `.pcap` files in that repository are stored in Git LFS, so a plain clone
gets 128-byte pointer files rather than captures. Fetch them from GitHub's
media endpoint:

```bash
mkdir -p /tmp/gsof-pcaps && cd /tmp/gsof-pcaps
base=https://media.githubusercontent.com/media/trimble-oss/trimble_driver_ros/main/trimble_driver/test/data
for f in applus60_gsof1 applus60_gsof2 applus60_gsof3 applus60_gsof6 applus60_gsof7 \
         applus60_gsof8 applus60_gsof9 applus60_gsof10 applus60_gsof11 applus60_gsof12 \
         applus60_gsof15 applus60_gsof16 applus60_gsof27 applus60_gsof33 applus60_gsof34 \
         applus60_gsof35 applus60_gsof37 applus60_gsof38 applus60_gsof40 applus60_gsof41 \
         apx_18_fullnavinfo_single apx_18_fullrmsinfo_single; do
  curl -sL -o "$f.pcap" "$base/$f.pcap"
done
```

then run `tools/gen_golden_records.py` (in this directory) against that
directory. It is not a build dependency: the generated header is checked in,
and regenerating is only needed when adding a record type.
