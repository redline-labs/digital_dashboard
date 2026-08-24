# Golden GSOF records

`golden_records.h` is generated. It holds the body of one record of each type,
taken from packet captures of real Trimble receivers published by Trimble.

There is no companion capture from our own receiver, and that is deliberate —
see [A capture of our own](#a-capture-of-our-own) below.

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

## A capture of our own

A whole transmission off our own receiver would be worth more than any of the
above, because it can be checked against ITSELF rather than against the ICD:
records 2, 3, 62 and 70 describe one position in four coordinate systems,
record 28's RTK age is record 38's correction age, record 48's pages carry
record 34's satellites plus the ones it had to truncate, and records 1, 16, 62
and 91 stamp the same instant through two opposite week/time-of-week orderings.
Each of those is two parsers agreeing about one physical fact, which no reading
of the specification can manufacture.

**A GSOF capture is a position fix, so where you take it matters.** Not only
from the position records — the satellite azimuths and elevations in records 33,
34 and 48, against the timestamp in record 1, pin the observer down on their
own. A capture taken at home or at a customer site should not be committed, and
scrubbing the position records is not enough to make one safe.

So capture somewhere you are content to publish, with the receiver set to emit
every message type it has and holding a fix:

```bash
bd992_bridge --config configs/bd992/bd992.yaml --dump-gsof /tmp/bd992.bin
# ... let it run a few seconds, then Ctrl-C

tools/gen_golden_bd992.py /tmp/bd992.bin "<date>, <place>" > golden_bd992.h
```

It takes the first two complete transmissions. The second argument is the
provenance line in the header comment, so put something you are happy to have
in the repository.

Records 13, 14, 28, 48, 62, 70, 74, 91, 92 and 96 were validated against a live
receiver this way, but that capture was taken privately and is not checked in.
What stands in for it in `tests/test_records.cpp` is a set of SYNTHETIC vectors,
labelled as such, that exercise the parsers' arithmetic — nested variable
lengths, counts that must agree with a record length, a name whose length is the
record length minus the fixed part. Those catch a logic error. They cannot catch
a field offset that is wrong in the same way in both the parser and the vector,
which is exactly what a real capture is for.

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

For a capture from our own receiver, see [A capture of our own](#a-capture-of-our-own).
