#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates the default manufacturer-button artwork referenced by carplay.yaml.
#
# The icons are committed alongside this script rather than rendered at build
# time -- the node should not need a Python toolchain to start. Re-run this only
# when the artwork changes:
#
#     python3 configs/carplay/make_oem_icon.py
#
# A steering wheel on the dashboard's slate, at the three scales CarPlay picks
# between. Drawn analytically with 4x supersampling, so there is no font or
# image dependency; PNG is emitted directly (zlib is the only thing needed).

import math
import struct
import zlib
from pathlib import Path

SIZES = (60, 120, 180)

BACKGROUND = (0x1C, 0x1C, 0x1E)
FOREGROUND = (0xF2, 0xF2, 0xF7)

# All geometry is in units of the icon's edge length so every size is identical
# artwork. The rim is a stroked circle; three spokes join it to a filled hub.
RIM_RADIUS = 0.375
RIM_WIDTH = 0.075
HUB_RADIUS = 0.115
SPOKE_WIDTH = 0.070
# Down, and up to either side: the classic three-spoke wheel.
SPOKE_ANGLES = (90.0, 210.0, 330.0)

SUPERSAMPLE = 4


def _coverage(px: float, py: float) -> float:
    """Fraction of the foreground at a point, in 0..1 unit coordinates."""
    x = px - 0.5
    y = py - 0.5
    r = math.hypot(x, y)

    if abs(r - RIM_RADIUS) <= RIM_WIDTH / 2.0:
        return 1.0
    if r <= HUB_RADIUS:
        return 1.0

    # A spoke is the segment from the hub edge out to the rim, so the test is
    # "close enough to the ray, and between the two radii".
    if HUB_RADIUS <= r <= RIM_RADIUS:
        for degrees in SPOKE_ANGLES:
            radians = math.radians(degrees)
            # Distance from the point to the spoke's centre line.
            across = -x * math.sin(radians) + y * math.cos(radians)
            along = x * math.cos(radians) + y * math.sin(radians)
            if along >= 0.0 and abs(across) <= SPOKE_WIDTH / 2.0:
                return 1.0
    return 0.0


def render(size: int) -> bytes:
    """Returns raw RGBA rows for one icon, ready to be filtered and deflated."""
    rows = bytearray()
    step = 1.0 / (size * SUPERSAMPLE)
    for row in range(size):
        rows.append(0)  # PNG filter type 0 (None) for this scanline
        for col in range(size):
            hits = 0
            for sub_y in range(SUPERSAMPLE):
                py = (row + (sub_y + 0.5) / SUPERSAMPLE) / size
                for sub_x in range(SUPERSAMPLE):
                    px = (col + (sub_x + 0.5) / SUPERSAMPLE) / size
                    if _coverage(px, py) > 0.0:
                        hits += 1
            alpha = hits / float(SUPERSAMPLE * SUPERSAMPLE)
            pixel = tuple(
                round(BACKGROUND[i] + (FOREGROUND[i] - BACKGROUND[i]) * alpha)
                for i in range(3)
            )
            rows.extend(pixel)
            rows.append(0xFF)  # opaque: CarPlay masks the square itself
    del step
    return bytes(rows)


def _chunk(tag: bytes, payload: bytes) -> bytes:
    body = tag + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def write_png(path: Path, size: int) -> None:
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    data = (
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", header)
        + _chunk(b"IDAT", zlib.compress(render(size), 9))
        + _chunk(b"IEND", b"")
    )
    path.write_bytes(data)
    print(f"{path} ({len(data)} bytes)")


def main() -> None:
    here = Path(__file__).resolve().parent
    for size in SIZES:
        write_png(here / f"oem_icon_{size}.png", size)


if __name__ == "__main__":
    main()
