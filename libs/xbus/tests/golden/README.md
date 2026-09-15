# XBus golden vectors

## The problem this directory does not solve

`libs/gsof/tests/golden/README.md` states the rule these vectors are judged
against:

> A byte vector authored from the same reading of the ICD as the parser agrees
> with the parser by construction — including everywhere both are wrong.

GSOF answers that with captures from real receivers. **There is no MTi-610 on
the bench**, so this directory cannot. What it does instead is keep the two
kinds of vector apart and label every one, so nobody reads a self-consistency
check as a confirmation of the wire format.

## VENDOR

Five complete messages, checksum included, printed by Xsens in the *MT Low
Level Communication Protocol Documentation* (MT0101P rev 2019.C). These came
out of Xsens' encoder rather than out of anybody's reading of the spec, and
they are the only bytes in this library with that property.

| Vector | Source | What it pins |
|---|---|---|
| `kReqDeviceId` | §5.2 | header layout, checksum, zero-length payload |
| `kReqBaudrate` | §5.2 | that a request is the zero-length form of its message id |
| `kSetBaudrateAck` | §5.2 | the acknowledgement rule, MID + 1 |
| `kSetStringOutputTypeNone` | §5.3.6 | the checksum across a two-byte payload |
| `kSetOptionFlagsEnableAhs` | §5.3.3, Table 9 | the checksum across an eight-byte payload, and SetFlags/ClearFlags order |
| `kFirmwareRevReply` | `bodypack_0_0.mtb` | that Table 4's FirmwareRev offsets are right |
| `kOutputConfigAckReply` | `bodypack_0_0.mtb` | the output-configuration entry layout |

The last two are a different kind of evidence again: nobody typeset them. An
Xsens device put them on a wire and Xsens' own software wrote them to a file.

Two further vendor values live in `test_fixed_point.cpp` rather than here,
because they are scalars and not messages: §5.1.1 prints `9.81` as the float32
`0x411CF5C3` and `1275` as the uint16 `0x04FB`.

`make_message()` reproduces all five byte for byte, which is what the framing
of every command in `commands.h` rests on.

## The SDK's sample logs

The `xsens-xme-sdk` package ships two multi-megabyte `.mtb` files, and an
`.mtb` is a raw XBus stream. Framing them end to end is the closest thing to a
capture this project has: if the checksum rule, the extended-length encoding or
the message-size arithmetic were wrong, the byte count would not come out even.

`tools/verify_sdk_corpus.py` does this with a second, independent framer written
in Python from the LLCP. Both files account for **every byte**:

| File | Bytes | Messages | Extended-length | Unaccounted |
|---|---|---|---|---|
| `bodypack_0_0.mtb` | 3,405,177 | 5,305 | 5,303 | 0 |
| `bodypack_1_0.mtb` | 6,422,777 | 10,020 | 10,018 | 0 |

The C++ `xbus::Framer` was run over the same corpus, fed in 37-byte reads so
message boundaries land mid-read the way they do on a serial port, and produced
**the same counts**, with zero checksum errors, zero resyncs, zero dropped bytes
and nothing left buffered.

That is what extended-length framing rests on. Before this it was covered only
by vectors written here, which is the weakest kind of evidence — and 15,321 of
these 15,325 messages use the long form.

The logs are from a Bodypack, not an MTi, so the DATA in them is not something
an MTi-610 would send. That does not matter for the claim being made: the frame
is common to every Xsens device. Two small complete messages extracted from them
are in `golden_messages.h` and are labelled VENDOR above.

**The logs are not checked in** — roughly 10 MB of Xsens-licensed sample data.
The script is how the table above gets re-checked:

```bash
mkdir -p /tmp/sdk && cd /tmp/sdk
ar p ~/Downloads/xsens-xme-sdk_*.deb data.tar.zst | zstd -dc | tar -x
python3 libs/xbus/tests/golden/tools/verify_sdk_corpus.py \
    /tmp/sdk/usr/share/doc/xsens-xme-sdk/examples/import_mtb_file
```

## SYNTHETIC

Everything else. Constructed here to exercise a case the LLCP does not print —
extended length, a payload containing a preamble byte, an MTData2 body, a
truncated header. They check that the parser is self-consistent and that it
refuses malformed input. **They cannot confirm the wire format.**

Two of the synthetic checksums in the first draft of this file were wrong, and
the four vendor vectors are what caught it. That is the argument for keeping
them separate, in one sentence.

## What carries weight instead

Three things stand in for the captures that do not exist, in descending order:

1. **The vendor vectors above.**
2. **The SDK's own enumerations.** Every message id and data identifier is
   transcribed from `xstypes/xsxbusmessageid.h` and `xstypes/xsdataidentifier.h`
   in the `xsens-xme-sdk` package, not from the LLCP's prose tables. Two
   independent renderings of the protocol have to agree.
3. **The dual-encoding cross-check** in `test_fixed_point.cpp` and
   `test_mtdata2.cpp`. One value, encoded in all four precisions, must decode to
   one answer. This is the only check that catches the fp16.32 byte swizzle:
   round-tripping cannot, because an encoder and decoder sharing the same
   mistake agree perfectly. Removing the swizzle breaks the build at four
   `static_assert`s — verified.

## Promoting these once hardware exists

```bash
# Capture a few seconds of the real stream.
mti610_bridge --config configs/mti610/mti610.yaml --dump-xbus /tmp/mti610.bin

# Regenerate the header from it.
python3 libs/xbus/tests/golden/tools/gen_golden.py /tmp/mti610.bin \
    > libs/xbus/tests/golden/golden_captured.h
```

Then, in order:

1. Add the captured vectors alongside the synthetic ones and mark them
   `CAPTURED`.
2. Delete each synthetic vector only once a captured one covers the same
   identifier — not before, because the synthetic ones also cover malformed
   input a healthy device never produces.
3. Re-check every item in the "deferred to hardware" list in `docs/nodes/mti610_bridge.md`.
   Several of them (the real sample rates, whether `AccelerationHR` arrives in
   its own packet, the `SampleTimeFine` wrap point) are answered by a long
   enough capture and by nothing else.

The generator is **not a build dependency**. The header is checked in;
regeneration is only needed when the vectors change.

**A capture of an IMU is not location-bearing the way a GNSS capture is**, so
the provenance rule that keeps BD992 captures out of the tree does not apply
here — but a capture taken while the device is bolted to a moving vehicle is a
trajectory. Check what is in it before committing it.
