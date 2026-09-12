#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Frame every XBus message in the sample logs shipped with the Xsens SDK and
# report whether the whole file is accounted for.
#
# WHY THIS EXISTS. With no MTi on the bench, the framing layer of libs/xbus was
# checked against five short messages printed in the LLCP and a handful of
# synthetic ones. The SDK ships two multi-megabyte .mtb logs that are raw XBus
# streams, and framing them end to end is the closest thing to a capture this
# project has: if the checksum rule, the extended-length encoding or the
# message-size arithmetic were wrong, the byte count would not come out even.
#
# The logs are from a Bodypack rather than an MTi, so the DATA in them is not
# something an MTi-610 would send. That is fine -- what is being verified is
# the frame, which is common to every Xsens device.
#
# NOT A BUILD DEPENDENCY, and the logs are NOT checked in: they are ~10 MB of
# Xsens-licensed sample data. Two small complete messages extracted from them
# live in ../golden_messages.h; this script is how the aggregate claim in
# ../README.md gets re-checked.
#
# Usage:
#     dpkg-deb -x xsens-xme-sdk_*.deb /tmp/sdk    # or ar + tar, see README
#     python3 verify_sdk_corpus.py /tmp/sdk/usr/share/doc/xsens-xme-sdk/examples/import_mtb_file
#
# The framing here is deliberately a SECOND implementation, written from the
# LLCP in Python rather than reusing the C++, so that a disagreement is worth
# looking at rather than a shared mistake.

import argparse
import collections
import pathlib
import sys

PREAMBLE = 0xFA
EXT_LEN = 0xFF
EXT_MID = 0xEE


def frame(data):
    """Yield (offset, mid, extended, total, payload) for each valid message."""
    i = 0
    while i < len(data):
        if data[i] != PREAMBLE or len(data) - i < 5:
            i += 1
            continue

        mid = data[i + 2]
        if mid == EXT_MID:
            # libs/xbus refuses these rather than guessing the layout; so does
            # this. See parse_message() for why.
            i += 1
            continue

        length = data[i + 3]
        at = i + 4
        extended = length == EXT_LEN

        if extended:
            if len(data) - i < 7:
                i += 1
                continue
            length = (data[i + 4] << 8) | data[i + 5]
            at = i + 6

        total = at - i + length + 1
        if len(data) - i < total:
            i += 1
            continue

        # Every byte from BID through the checksum must sum to zero mod 256.
        if sum(data[i + 1:i + total]) & 0xFF != 0:
            i += 1
            continue

        yield i, mid, extended, total, data[at:at + length]
        i += total


def check(path):
    data = path.read_bytes()
    messages = list(frame(data))

    if not messages:
        print(f"{path.name}: no valid XBus messages", file=sys.stderr)
        return False

    consumed = sum(m[3] for m in messages)
    # Bytes not inside any accepted message. A correct framer should leave
    # none: the file is a raw stream with nothing between messages.
    skipped = len(data) - consumed
    extended = sum(1 for m in messages if m[2])
    mids = collections.Counter(m[1] for m in messages)

    print(f"{path.name}")
    print(f"  {len(data):>10,} bytes")
    print(f"  {len(messages):>10,} messages ({extended:,} extended-length)")
    print(f"  {skipped:>10,} bytes not accounted for")
    print( "  MIDs: " + ", ".join(f"0x{m:02X}x{c}" for m, c in mids.most_common(8)))

    if skipped:
        print(f"  FAIL: {skipped} bytes are outside any valid message", file=sys.stderr)
        return False

    print("  OK: the whole file frames cleanly")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="directory holding the SDK's .mtb sample logs")
    args = parser.parse_args()

    logs = sorted(pathlib.Path(args.directory).glob("*.mtb"))
    if not logs:
        print(f"no .mtb files in {args.directory}", file=sys.stderr)
        return 1

    return 0 if all([check(p) for p in logs]) else 1


if __name__ == "__main__":
    sys.exit(main())
