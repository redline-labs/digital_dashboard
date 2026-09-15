#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates the synthetic test DBCs under tests/dbcs/ and the cantools goldens
# for them, as an embedded C++ header.
#
# Why synthetic rather than the DBCs the product ships: pinning values from a
# real DBC couples the test suite to files that evolve for product reasons, so
# adding a signal to Motec_M1_Rev3.dbc would break a parser test. These inputs
# never change, so their goldens never need regenerating.
#
# That trade is only sound if the synthetic corpus covers the layout space
# exhaustively -- a round-trip bug in a real DBC has to be impossible unless
# the corpus would also have caught it. So the sweep below is *generated* over
# every combination of byte order, signedness, length and bit alignment rather
# than hand-picked, and the counts it prints are the coverage argument.
#
# cantools is NOT a build dependency. Everything this writes is checked in:
#
#     python3 -m venv /tmp/ct && /tmp/ct/bin/pip install cantools==44.0.0
#     /tmp/ct/bin/python libs/dbc_parser/tests/gen_golden.py
#
# Values are emitted with decode_choices=False, so a signal with a value table
# is compared as a number and the test is not coupled to enumerator naming.
#
# Two things cantools does differently from the generated code, and which the
# goldens therefore stay away from:
#   * it rounds a physical value to raw with Python's round(), half to even;
#     the generated encoder rounds half away from zero. No golden encode starts
#     from a value near a tie.
#   * it raises on a value outside the field instead of saturating. No golden
#     encode starts from a value outside the field.
# Both behaviours are pinned by hand in test_precision.cpp instead.

import itertools
import math
import pathlib
import random
import struct
import sys

import cantools

CANTOOLS_VERSION = "44.0.0"

HERE = pathlib.Path(__file__).resolve().parent
DBC_DIR = HERE / "dbcs"
GOLDEN_HEADER = HERE / "golden_data.h"

FRAMES_PER_MESSAGE = 48
FRAMES_PER_MUX_GROUP = 16
REJECTION_BUDGET = 400
PHYSICAL_ENCODES_PER_MESSAGE = 32

# How far a generated physical value may sit from a raw step, in steps. A tie is
# half a step away, so staying within a quarter keeps every rounding decision
# the same under half-to-even and half-away-from-zero, and far outside the error
# a float32 decode is allowed (see the R' threshold in docs/libs/dbc_parser.md).
PHYSICAL_JITTER_STEPS = 0.25

FRAME_BITS = 64  # every synthetic message is a full 8 byte frame


# --------------------------------------------------------------------------
# Bit layout
# --------------------------------------------------------------------------

def covered_bits(start, length, little_endian):
    """The exact bit indices a signal occupies, or None if it leaves the frame.

    This mirrors the walk the generated decoder performs, including the
    Motorola sawtooth, so a placement accepted here is one the decoder can
    actually index.
    """
    bits = []
    if little_endian:
        for i in range(length):
            bit = start + i
            if bit >= FRAME_BITS:
                return None
            bits.append(bit)
        return bits

    bit = start
    for _ in range(length):
        if bit >= FRAME_BITS or bit < 0:
            return None
        bits.append(bit)
        bit = bit + 15 if bit % 8 == 0 else bit - 1
    return bits


class Frame:
    """Places signals into a frame without overlap."""

    def __init__(self):
        self.used = set()
        self.signals = []

    def place(self, name, length, little_endian, signed, scale, offset, unit=""):
        for start in range(FRAME_BITS):
            bits = covered_bits(start, length, little_endian)
            if bits is None or self.used.intersection(bits):
                continue
            self.used.update(bits)
            self.signals.append({
                "name": name,
                "start": start,
                "length": length,
                "little_endian": little_endian,
                "signed": signed,
                "scale": scale,
                "offset": offset,
                "unit": unit,
            })
            return True
        return False

    def must_place(self, *args, **kwargs):
        # A hand-built message whose signal does not fit is a broken test input,
        # not something to skip: carrying on silently once relabelled the wrong
        # signal as a multiplexed group.
        if not self.place(*args, **kwargs):
            raise SystemExit(f"cannot place {args[0]}: frame is full")
        return self.signals[-1]


def field_range(length, signed):
    if signed:
        return -(2 ** (length - 1)), 2 ** (length - 1) - 1
    return 0, 2 ** length - 1


def signal_line(sig, mux=None):
    lo, hi = field_range(sig["length"], sig["signed"])
    # The declared range has to cover every physical value the field can hold,
    # or cantools refuses to encode even with strict=False disabled elsewhere.
    physical = sorted([lo * sig["scale"] + sig["offset"], hi * sig["scale"] + sig["offset"]])
    marker = f" {mux}" if mux else ""
    return (f' SG_ {sig["name"]}{marker} : {sig["start"]}|{sig["length"]}'
            f'@{1 if sig["little_endian"] else 0}{"-" if sig["signed"] else "+"}'
            f' ({sig["scale"]},{sig["offset"]})'
            f' [{physical[0]}|{physical[1]}] "{sig["unit"]}" ECU')


def write_dbc(path, messages, extra_lines=()):
    lines = [
        'VERSION "synthetic -- generated by libs/dbc_parser/tests/gen_golden.py"',
        "",
        "NS_ :",
        "",
        "BS_:",
        "",
        "BU_ : ECU",
        "",
    ]
    for msg in messages:
        lines.append(f'BO_ {msg["id"]} {msg["name"]}: 8 ECU')
        for sig in msg["signals"]:
            lines.append(signal_line(sig, sig.get("mux")))
        lines.append("")
    lines.extend(extra_lines)
    path.write_text("\n".join(lines) + "\n")


def pack(specs, base_id, prefix):
    """First-fit a list of (name, length, little, signed, scale, offset) into
    as many 8 byte messages as it takes. Returns (messages, placed)."""
    messages = []
    frame = Frame()
    index = 0
    placed = 0
    for spec in specs:
        if not frame.place(*spec):
            messages.append({"id": base_id + index, "name": f"{prefix}{index}", "signals": frame.signals})
            index += 1
            frame = Frame()
            if not frame.place(*spec):
                continue
        placed += 1
    if frame.signals:
        messages.append({"id": base_id + index, "name": f"{prefix}{index}", "signals": frame.signals})
    return messages, placed


# --------------------------------------------------------------------------
# The sweeps
# --------------------------------------------------------------------------

# Lengths chosen to straddle every boundary the bit walk cares about: single
# bit, sub-byte, byte, byte+1, and the 32/53/64 bit points where integer and
# double representations stop agreeing.
LENGTHS = [1, 2, 3, 7, 8, 9, 12, 15, 16, 17, 24, 31, 32, 33, 53, 63, 64]


def build_layout_dbc():
    """Byte order x signedness x length, with identity scaling.

    Isolates the bit walk and sign extension: any failure here is the decoder
    reading the wrong bits, not arithmetic.
    """
    combos = list(itertools.product([True, False], [False, True], LENGTHS))
    specs = [(f'L{length}_{"LE" if little else "BE"}_{"S" if signed else "U"}', length, little, signed, 1, 0)
             for little, signed, length in combos]
    messages, placed = pack(specs, 0x100, "Layout")
    return messages, placed, len(combos)


# Scale and offset combinations, chosen so every branch of the generator's type
# selection is exercised: integral vs fractional, positive vs negative, and the
# unsigned-field-with-negative-offset case that used to convert out of range.
SCALINGS = [
    (1, 0),        # identity -> integer type
    (1, -40),      # unsigned field, negative offset -> must become signed
    (1, 100),      # integral positive offset
    (0.1, 0),      # fractional scale -> double
    (0.5, -273.15),  # fractional both
    (-1, 0),       # negative scale -> must become signed
    (-0.25, 10),   # negative fractional scale
    (2, 0),        # integral scale > 1
    (0.001, 0),    # small scale
    (1000, 0),     # large scale
]


def build_scaling_dbc():
    """Scale x offset x signedness, at a couple of fixed widths."""
    combos = list(itertools.product(SCALINGS, [False, True], [8, 16], [True, False]))
    specs = []
    for (scale, offset), signed, length, little in combos:
        tag = f"S{SCALINGS.index((scale, offset))}"
        specs.append((f'{tag}_{length}_{"LE" if little else "BE"}_{"S" if signed else "U"}',
                      length, little, signed, scale, offset))
    messages, placed = pack(specs, 0x200, "Scaling")
    return messages, placed, len(combos)


def build_features_dbc():
    """Multiplexing, value tables, IEEE floats and extended identifiers."""
    messages = []
    extra = []

    # --- multiplexing: four groups, plus a signal outside the multiplex ---
    # Each group gets a differently shaped signal so the gating and the bit walk
    # are exercised together rather than one masking the other. Placed widest
    # first: first-fit in declaration order fragments the frame so the 12 bit
    # group no longer fits after the 16 bit one.
    mux_frame = Frame()
    mux_frame.must_place("MuxIndex", 4, False, False, 1, 0)["mux"] = "M"
    mux_frame.must_place("Always", 8, False, False, 1, 0)
    shapes = {0: (8, True, False), 1: (16, False, False), 2: (12, True, True), 3: (3, False, True)}
    for group in sorted(shapes, key=lambda g: -shapes[g][0]):
        length, little, signed = shapes[group]
        mux_frame.must_place(f"Group{group}", length, little, signed, 1, 0)["mux"] = f"m{group}"
    messages.append({"id": 0x300, "name": "Multiplexed", "signals": mux_frame.signals})

    # --- value tables, including the naming cases that used to break ---
    values_frame = Frame()
    values_frame.must_place("Plain", 8, True, False, 1, 0)
    values_frame.must_place("Colliding", 8, True, False, 1, 0)
    values_frame.must_place("Awkward", 8, True, False, 1, 0)
    messages.append({"id": 0x301, "name": "WithValues", "signals": values_frame.signals})
    extra.extend([
        'VAL_ 769 Plain 0 "Off" 1 "On" 2 "Standby" ;',
        # Two entries that sanitise to the same identifier.
        'VAL_ 769 Colliding 0 "Fault-A" 1 "Fault_A" 2 "Fault A" ;',
        # Leading digit, empty, reserved word, and punctuation only.
        'VAL_ 769 Awkward 0 "0 to 100%" 1 "" 2 "class" 3 "---" ;',
        "",
    ])

    # --- IEEE float and double via SIG_VALTYPE_ ---
    float_frame = Frame()
    float_frame.must_place("AsFloat", 32, True, True, 1, 0)
    float_frame.must_place("AsInt", 32, True, False, 1, 0)
    messages.append({"id": 0x302, "name": "FloatSignals", "signals": float_frame.signals})

    double_frame = Frame()
    double_frame.must_place("AsDouble", 64, True, True, 1, 0)
    messages.append({"id": 0x303, "name": "DoubleSignal", "signals": double_frame.signals})

    extra.extend([
        "SIG_VALTYPE_ 770 AsFloat : 1;",
        "SIG_VALTYPE_ 771 AsDouble : 2;",
        "",
    ])

    # --- extended identifier: bit 31 set is how a DBC spells 29 bit ---
    ext_frame = Frame()
    ext_frame.must_place("Payload", 32, True, False, 1, 0)
    messages.append({
        "id": 0x80000000 | 0x18FEEE00,
        "name": "ExtendedId",
        "signals": ext_frame.signals,
    })

    # --- comment containing a quote, which must survive into generated code ---
    extra.extend([
        r'CM_ SG_ 769 Plain "a comment with \"quotes\" and a \\ backslash";',
        r'CM_ BO_ 768 "multiplexed message";',
        "",
    ])

    return messages, extra


# Signals that sit on either side of every type-selection boundary the
# generator draws: float vs double at R' = max|raw| + |offset/scale| = 2^20,
# and each integer width a scaled field can need. Scales and offsets have exact
# binary ratios where they straddle the threshold, so which side a signal lands
# on does not depend on rounding in the generator.
#   name, length, little_endian, signed, scale, offset
PRECISION_SIGNALS = [
    # float side: R' < 2^20
    ("F20U_Tenth", 20, True, False, 0.1, 0),             # R' = 2^20 - 1
    ("F12U_QuarterOffset", 12, False, False, 0.25, 261120),  # R' = 4095 + 1044480 = 2^20 - 1
    ("F16S_Milli", 16, True, True, 0.001, 0),
    ("F16U_NegHalf", 16, False, False, -0.5, 100),
    ("F8U_Kelvin", 8, True, False, 0.5, -273.15),
    # double side: R' >= 2^20
    ("D21S_Tenth", 21, True, True, 0.1, 0),              # R' = 2^20
    ("D20U_HalfHalf", 20, False, False, 0.5, 0.5),       # R' = (2^20 - 1) + 1
    ("D12U_QuarterOffset", 12, True, False, 0.25, 261120.25),  # R' = 4095 + 1044481 = 2^20
    ("D24U_Centi", 24, False, False, 0.01, 0),
    ("D32S_CentiOffset", 32, True, True, 0.01, -5),
    # integer widths a scaled field needs
    ("I32U_Times4", 32, False, False, 4, 0),             # up to 17 179 869 180
    ("I32S_NegThreeOffset", 32, True, True, -3, 7),
    ("I16U_Thousand", 16, False, False, 1000, 0),        # up to 65 535 000
    ("I8S_NegTwo", 8, True, True, -2, 0),
    ("I3S_FiveOffset", 3, False, True, 5, -1),
    ("B1U_Flag", 1, True, False, 1, 0),
    ("I1U_Offset", 1, False, False, 1, 5),
    # IEEE float with scaling applied on top
    ("IeeeScaled", 32, True, True, 0.5, 10),
]


def build_precision_dbc():
    messages, placed = pack(PRECISION_SIGNALS, 0x400, "Precision")
    extra = []
    for msg in messages:
        for sig in msg["signals"]:
            if sig["name"].startswith("Ieee"):
                extra.append(f'SIG_VALTYPE_ {msg["id"]} {sig["name"]} : 1;')
    extra.append("")
    return messages, extra, placed


DATABASES = []


def emit_dbcs():
    DBC_DIR.mkdir(exist_ok=True)

    layout, placed, total = build_layout_dbc()
    write_dbc(DBC_DIR / "dbc_test_layout.dbc", layout)
    print(f"layout:    {len(layout)} messages, {placed}/{total} byte-order x sign x length combos")

    scaling, placed, total = build_scaling_dbc()
    write_dbc(DBC_DIR / "dbc_test_scaling.dbc", scaling)
    print(f"scaling:   {len(scaling)} messages, {placed}/{total} scale x offset x sign x width combos")

    features, extra = build_features_dbc()
    write_dbc(DBC_DIR / "dbc_test_features.dbc", features, extra)
    print(f"features:  {len(features)} messages (multiplexing, value tables, floats, extended id)")

    precision, extra, placed = build_precision_dbc()
    write_dbc(DBC_DIR / "dbc_test_precision.dbc", precision, extra)
    print(f"precision: {len(precision)} messages, {placed}/{len(PRECISION_SIGNALS)} type-boundary signals")
    if placed != len(PRECISION_SIGNALS):
        raise SystemExit("a precision signal did not fit")

    DATABASES.extend([
        ("dbc_test_layout", DBC_DIR / "dbc_test_layout.dbc", False),
        ("dbc_test_scaling", DBC_DIR / "dbc_test_scaling.dbc", False),
        ("dbc_test_features", DBC_DIR / "dbc_test_features.dbc", False),
        ("dbc_test_precision", DBC_DIR / "dbc_test_precision.dbc", True),
    ])


# --------------------------------------------------------------------------
# Goldens
# --------------------------------------------------------------------------

def multiplex_groups(message):
    if not message.is_multiplexed():
        return None
    for node in message.signal_tree:
        if isinstance(node, dict):
            name, groups = next(iter(node.items()))
            return name, sorted(groups.keys())
    return None


def fmt(value):
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    return repr(float(value))


def raw_bits(signal, raw_value):
    """The raw field as its bit pattern, masked to the field.

    Written as bits rather than as a number so that signed fields, unsigned
    fields and IEEE floats all compare the same way on the C++ side. cantools'
    unscaled value for an IEEE signal is the float itself, not its bits.
    """
    if signal.is_float:
        if signal.length == 32:
            return struct.unpack("<I", struct.pack("<f", raw_value))[0]
        return struct.unpack("<Q", struct.pack("<d", raw_value))[0]
    return int(raw_value) & ((1 << signal.length) - 1)


def is_integral(value):
    return float(value).is_integer()


def raw_survives_physical(signal, physical, raw_value):
    """Whether the raw bits can be recovered from the physical value at all.

    Always true for an integer field. Not for an IEEE float with scaling: a
    float32 of 4e-33 times 0.5 plus 10 is exactly 10.0 in a double, so no
    decoder can hand the raw bits back. The raw column is left off those values
    rather than asserting something no implementation could satisfy; the value
    and the re-encode are still checked.
    """
    if not signal.is_float or (signal.scale == 1 and signal.offset == 0):
        return True
    try:
        back = (physical - signal.offset) / signal.scale
        return raw_bits(signal, back) == raw_bits(signal, raw_value)
    except (OverflowError, struct.error):
        return False


def decodes_cleanly(db, message, payload):
    """False for a payload that puts a NaN in an IEEE signal.

    A NaN cannot be compared as a value, and converting a float32 NaN through a
    Python double does not reliably keep its payload bits, so the raw column
    could not be trusted either.
    """
    raw = db.decode_message(message.frame_id, payload, decode_choices=False, scaling=False)
    return not any(isinstance(v, float) and math.isnan(v) for v in raw.values())


def sample_payloads(db, message, rng):
    groups = multiplex_groups(message)
    if groups is None:
        produced = 0
        budget = REJECTION_BUDGET * FRAMES_PER_MESSAGE
        while produced < FRAMES_PER_MESSAGE and budget > 0:
            budget -= 1
            payload = bytes(rng.getrandbits(8) for _ in range(message.length))
            if not decodes_cleanly(db, message, payload):
                continue
            produced += 1
            yield payload
        return

    mux_name, group_ids = groups
    wanted = {group_id: FRAMES_PER_MUX_GROUP for group_id in group_ids}
    budget = REJECTION_BUDGET * FRAMES_PER_MUX_GROUP * max(len(group_ids), 1)

    while budget > 0 and any(count > 0 for count in wanted.values()):
        budget -= 1
        payload = bytes(rng.getrandbits(8) for _ in range(message.length))
        try:
            group_id = message.decode(payload, decode_choices=False)[mux_name]
        except Exception:
            continue
        if wanted.get(group_id, 0) <= 0:
            continue
        wanted[group_id] -= 1
        yield payload

    starved = [g for g, count in wanted.items() if count > 0]
    if starved:
        print(f"  warning: {message.name} under-sampled mux groups {starved}", file=sys.stderr)


def boundary_payloads(db, message):
    """Every field at raw_min, raw_max, 0, +1 and -1 at once.

    Random payloads almost never hit the ends of a wide field, and the ends are
    where a float32 decode has the least precision to spare.
    """
    patterns = []
    for pattern in ("min", "max", "zero", "plus_one", "minus_one"):
        raw = {}
        for signal in message.signals:
            lo, hi = field_range(signal.length, signal.is_signed)
            if signal.is_float:
                limit = 3.4028234663852886e38 if signal.length == 32 else sys.float_info.max
                value = {"min": -limit, "max": limit, "zero": 0.0, "plus_one": 1.0, "minus_one": -1.0}[pattern]
            else:
                value = {"min": lo, "max": hi, "zero": 0, "plus_one": min(1, hi), "minus_one": max(-1, lo)}[pattern]
            raw[signal.name] = value
        patterns.append(db.encode_message(message.frame_id, raw, scaling=False, strict=False))
    return patterns


def active_signals(message, group):
    for signal in message.signals:
        if signal.multiplexer_ids is None or group in signal.multiplexer_ids:
            yield signal


def physical_value(signal, rng):
    """A random in-range physical value that is not near a rounding tie."""
    lo, hi = field_range(signal.length, signal.is_signed)
    if signal.is_float:
        while True:
            bits = rng.getrandbits(signal.length)
            fmt_bits, fmt_float = ("<I", "<f") if signal.length == 32 else ("<Q", "<d")
            raw = struct.unpack(fmt_float, struct.pack(fmt_bits, bits))[0]
            if math.isfinite(raw):
                return raw * signal.scale + signal.offset
    raw = rng.randint(lo, hi)
    if is_integral(signal.scale) and is_integral(signal.offset):
        # An integer-typed field cannot hold a fractional value, so there is
        # nothing to round: hand over the exact physical value.
        return raw * int(signal.scale) + int(signal.offset)
    q = raw + rng.uniform(-PHYSICAL_JITTER_STEPS, PHYSICAL_JITTER_STEPS)
    q = min(max(q, lo), hi)
    return q * signal.scale + signal.offset


def emit_database(lib_name, dbc_path, with_boundaries):
    db = cantools.database.load_file(dbc_path)

    lines = [f"database {lib_name}"]
    decode_cases = 0
    encode_cases = 0
    physical_cases = 0

    for message in db.messages:
        rng = random.Random(message.frame_id)
        lines.append(f"message 0x{message.frame_id:08X} {message.name} {message.length}")

        payloads = list(sample_payloads(db, message, rng))
        if with_boundaries:
            payloads = boundary_payloads(db, message) + payloads

        for payload in payloads:
            try:
                decoded = db.decode_message(message.frame_id, payload, decode_choices=False)
                raw = db.decode_message(message.frame_id, payload, decode_choices=False, scaling=False)
            except Exception:
                continue

            lines.append(f"decode {payload.hex()}")
            for name, value in sorted(decoded.items()):
                signal = message.get_signal_by_name(name)
                if raw_survives_physical(signal, value, raw[name]):
                    lines.append(f"  {name} {fmt(value)} 0x{raw_bits(signal, raw[name]):x}")
                else:
                    lines.append(f"  {name} {fmt(value)}")
            decode_cases += 1

            try:
                reencoded = db.encode_message(message.frame_id, decoded, strict=False)
            except Exception:
                continue

            lines.append(f"encode {reencoded.hex()}")
            encode_cases += 1

        # Encodes that start from a physical value rather than from a decode, so
        # the scale-and-round path is driven by values that are not already
        # exact multiples of the scale.
        groups = multiplex_groups(message)
        for _ in range(PHYSICAL_ENCODES_PER_MESSAGE):
            group = rng.choice(groups[1]) if groups else None
            values = {}
            for signal in active_signals(message, group):
                if groups and signal.name == groups[0]:
                    values[signal.name] = group
                else:
                    values[signal.name] = physical_value(signal, rng)
            try:
                encoded = db.encode_message(message.frame_id, values, strict=False)
            except Exception:
                continue
            lines.append(f"encode_physical {encoded.hex()}")
            for name, value in sorted(values.items()):
                lines.append(f"  {name} {fmt(value)}")
            physical_cases += 1

    print(f"  {lib_name}: {decode_cases} decode, {encode_cases} encode, {physical_cases} physical encode cases")
    return lines, decode_cases, encode_cases, physical_cases


def main():
    if cantools.__version__ != CANTOOLS_VERSION:
        raise SystemExit(f"cantools {cantools.__version__} found; the goldens are pinned to "
                         f"{CANTOOLS_VERSION}, whose rounding and saturation behaviour the "
                         "header comment describes")

    emit_dbcs()
    print()

    blocks = []
    totals = [0, 0, 0]
    for lib_name, dbc_path, with_boundaries in DATABASES:
        lines, *counts = emit_database(lib_name, dbc_path, with_boundaries)
        totals = [a + b for a, b in zip(totals, counts)]
        body = "\n".join(lines)
        blocks.append(
            f'// {lib_name}\n'
            f'inline constexpr const char* k_{lib_name} = R"GOLDEN(\n{body}\n)GOLDEN";\n'
        )

    header = (
        "#pragma once\n"
        "\n"
        "// Generated by libs/dbc_parser/tests/gen_golden.py -- do not edit.\n"
        "//\n"
        "// cantools' answers for the synthetic DBCs under tests/dbcs/. Embedded\n"
        "// rather than loaded from disk so the test carries its own data and needs\n"
        "// no path handed to it at build time.\n"
        "//\n"
        f"// cantools {CANTOOLS_VERSION}. {totals[0]} decode cases, {totals[1]} encode cases,\n"
        f"// {totals[2]} physical encode cases.\n"
        "\n"
        "namespace golden_data\n"
        "{\n"
        "\n" + "\n".join(blocks) + "\n"
        "}  // namespace golden_data\n"
    )

    GOLDEN_HEADER.write_text(header)
    print(f"\n{GOLDEN_HEADER.name}: {totals[0]} decode, {totals[1]} encode, {totals[2]} physical encode cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
