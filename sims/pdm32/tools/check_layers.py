#!/usr/bin/env python3
"""Include direction between sims/pdm32's layers.

Each file under board/, plant/, config/ and top/ may include only headers of the layers it sits on. Every quoted
include is resolved the way the compiler does (the including file's own directory, then the include root sims/) and
the layer of the file it reaches is checked, however the path is spelled. Includes that reach nothing under
sims/pdm32 (the libraries, the standard library) are not layered: libs/emu, libs/hc08 and libs/gz60 are separate
targets, so an upward include between them does not compile at all.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ALLOWED: dict[str, set[str]] = {
    "plant": {"plant"},
    "config": {"config"},
    "board": {"board"},
    "top": {"board", "plant", "config", "top"},
}
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def resolve(root: Path, includer: Path, name: str) -> Path | None:
    for directory in (includer.parent, root.parent):
        path = Path(os.path.normpath(directory / name))
        if path.is_file():
            return path
    return None


def layer_of(root: Path, path: Path) -> str | None:
    try:
        relative = path.relative_to(root)
    except ValueError:
        return None
    return relative.parts[0] if len(relative.parts) > 1 else None


def violations(root: Path) -> list[str]:
    found: list[str] = []
    for layer, allowed in ALLOWED.items():
        for path in sorted((root / layer).rglob("*")):
            if path.suffix not in (".cpp", ".h", ".inc"):
                continue
            for line in path.read_text().splitlines():
                m = INCLUDE.match(line)
                if not m:
                    continue
                target = resolve(root, path, m.group(1))
                reached = layer_of(root, target) if target else None
                if reached in ALLOWED and reached not in allowed:
                    found.append(f"{path.relative_to(root)} -> {target.relative_to(root)}")
    return found


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    found = violations(root)
    for v in found:
        print(f"upward include: {v}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
