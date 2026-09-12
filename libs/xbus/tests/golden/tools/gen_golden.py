#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Turn an `mti610_bridge --dump-xbus` capture into a checked-in C++ header of
# constexpr byte arrays.
#
# NOT A BUILD DEPENDENCY. The generated header is checked in; this is only run
# when the vectors change. See ../README.md.
#
# The framing here is deliberately a SECOND implementation of the one in
# libs/xbus -- written from the LLCP in Python rather than reusing the C++ --
# so that a capture it accepts and the library rejects is a disagreement worth
# looking at rather than a shared mistake.

import argparse
import collections
import sys

PREAMBLE = 0xFA
EXT_LEN = 0xFF


def messages(data):
    """Yield (offset, mid, payload) for every checksum-valid message."""
    i = 0
    while i < len(data):
        if data[i] != PREAMBLE or len(data) - i < 5:
            i += 1
            continue

        mid = data[i + 2]
        length = data[i + 3]
        at = i + 4

        if length == EXT_LEN:
            if len(data) - i < 7:
                i += 1
                continue
            length = (data[i + 4] << 8) | data[i + 5]
            at = i + 6

        total = at - i + length + 1
        if len(data) - i < total:
            i += 1
            continue

        if sum(data[i + 1:i + total]) & 0xFF != 0:
            i += 1
            continue

        yield i, mid, data[at:at + length]
        i += total


# The subset of xbus/data_table.h worth naming in a generated header. Kept in
# step by hand: a capture holding an identifier absent here is reported so the
# omission is visible rather than silent.
DATA_NAMES = {
    0x0810: "Temperature", 0x1010: "UtcTime", 0x1020: "PacketCounter",
    0x1060: "SampleTimeFine", 0x1070: "SampleTimeCoarse", 0x3010: "BaroPressure",
    0x4010: "DeltaV", 0x4020: "Acceleration", 0x4040: "AccelerationHr",
    0x8020: "RateOfTurn", 0x8030: "DeltaQ", 0x8040: "RateOfTurnHr",
    0xC020: "MagneticField", 0xE010: "StatusByte", 0xE020: "StatusWord",
}


def items(payload):
    """Walk an MTData2 body, yielding (raw_id, item_payload)."""
    i = 0
    while i + 3 <= len(payload):
        raw_id = (payload[i] << 8) | payload[i + 1]
        length = payload[i + 2]
        if i + 3 + length > len(payload):
            return
        yield raw_id, payload[i + 3:i + 3 + length]
        i += 3 + length


def emit_array(name, data, comment):
    print(f"// {comment}")
    print(f"inline constexpr std::array<std::uint8_t, {len(data)}> {name} {{")
    for start in range(0, len(data), 12):
        row = ", ".join(f"0x{b:02X}" for b in data[start:start + 12])
        print(f"    {row},")
    print("};")
    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", help="raw bytes from --dump-xbus")
    parser.add_argument("--source", default=None,
                        help="what produced the capture, for the header comment")
    args = parser.parse_args()

    with open(args.capture, "rb") as handle:
        data = handle.read()

    found = list(messages(data))
    if not found:
        print(f"no valid XBus messages in {args.capture}", file=sys.stderr)
        return 1

    source = args.source or args.capture

    # One example of each data identifier, taken from the first MTData2 that
    # carries it. One is enough: these pin a LAYOUT, and a second sample of the
    # same layout adds nothing a test can assert.
    examples = collections.OrderedDict()
    whole = None
    for _, mid, payload in found:
        if mid != 0x36:
            continue
        if whole is None:
            whole = payload
        for raw_id, item in items(payload):
            examples.setdefault(raw_id, item)

    print("// SPDX-License-Identifier: GPL-3.0-or-later")
    print("//")
    print("// GENERATED -- do not edit. Regenerate with the command in README.md.")
    print("//")
    print(f"// CAPTURED from {source}: {len(found)} messages.")
    print("// These are the ITEM payloads: the identifier and length bytes are")
    print("// stripped, because that is what a parse() is handed.")
    print()
    print("#ifndef XBUS_GOLDEN_CAPTURED_H")
    print("#define XBUS_GOLDEN_CAPTURED_H")
    print()
    print("#include <array>")
    print("#include <cstdint>")
    print()
    print("namespace xbus::golden")
    print("{")
    print()

    if whole:
        emit_array("kCapturedMtData2Body", whole,
                   "A whole MTData2 body: one sample, as the device grouped it.")

    unnamed = []
    for raw_id, item in examples.items():
        base = raw_id & 0xFFF0
        name = DATA_NAMES.get(base)
        if name is None:
            unnamed.append(raw_id)
            continue
        emit_array(f"kCaptured{name}", item,
                   f"XDI 0x{raw_id:04X} ({name}), format nibble 0x{raw_id & 0xF:X}")

    print("} // namespace xbus::golden")
    print()
    print("#endif // XBUS_GOLDEN_CAPTURED_H")

    if unnamed:
        ids = ", ".join(f"0x{i:04X}" for i in unnamed)
        print(f"capture holds identifiers not in DATA_NAMES: {ids}", file=sys.stderr)
        print("this device is not an MTi-610, or data_table.h is short a row",
              file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
