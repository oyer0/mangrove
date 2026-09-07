#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert the upstream regular UNSCII-16 HEX font to PSF2.

The upstream HEX file contains a mixture of 8x8, 8x16, and 16x16 entries.
Mangrove's console is an 8x16 monospace terminal, so this deliberately keeps
the upstream 8x16 entries and emits them without changing their bitmaps.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


MAGIC = b"\x72\xb5\x4a\x86"
PSF2_HEADER_SIZE = 32
PSF2_UNICODE_TABLE = 0x1
GLYPH_WIDTH = 8
GLYPH_HEIGHT = 16
GLYPH_BYTES = 16
MAX_SOURCE_BYTES = 512 * 1024
MAX_GLYPHS = 4096


def parse_codepoint(token: str, path: Path, line_number: int) -> int:
    try:
        value = int(token, 16)
    except ValueError as error:
        raise ValueError(f"{path}:{line_number}: invalid code point") from error
    if value > 0x10FFFF or 0xD800 <= value <= 0xDFFF:
        raise ValueError(f"{path}:{line_number}: invalid Unicode scalar")
    return value


def parse_source(path: Path) -> list[tuple[int, bytes]]:
    raw = path.read_bytes()
    if len(raw) > MAX_SOURCE_BYTES:
        raise ValueError("UNSCII source exceeds bounded input size")

    entries: list[tuple[int, bytes]] = []
    seen: set[int] = set()
    for line_number, raw_line in enumerate(raw.decode("ascii").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if ":" not in line:
            raise ValueError(f"{path}:{line_number}: expected codepoint:bitmap")
        codepoint_token, bitmap = line.split(":", 1)
        # The regular source includes other cell sizes.  Keep only the exact
        # 8x16 entries that fit Mangrove's existing terminal geometry.
        if len(bitmap) != GLYPH_BYTES * 2:
            continue
        codepoint = parse_codepoint(codepoint_token, path, line_number)
        try:
            glyph = bytes.fromhex(bitmap)
        except ValueError as error:
            raise ValueError(f"{path}:{line_number}: invalid bitmap") from error
        if len(glyph) != GLYPH_BYTES:
            raise ValueError(f"{path}:{line_number}: invalid 8x16 bitmap")
        if codepoint in seen:
            raise ValueError(f"{path}:{line_number}: duplicate 8x16 code point")
        seen.add(codepoint)
        entries.append((codepoint, glyph))

    if not entries or len(entries) > MAX_GLYPHS:
        raise ValueError(f"8x16 glyph count is outside 1..{MAX_GLYPHS}")
    required = list(range(0x20, 0x7F)) + [0x2500, 0x2502, 0x2514, 0x251C]
    missing = [f"U+{value:04X}" for value in required if value not in seen]
    if missing:
        raise ValueError("required mappings missing: " + ", ".join(missing))
    return entries


def build(source: Path, output: Path) -> None:
    entries = parse_source(source)
    header = struct.pack(
        "<4s7I",
        MAGIC,
        0,
        PSF2_HEADER_SIZE,
        PSF2_UNICODE_TABLE,
        len(entries),
        GLYPH_BYTES,
        GLYPH_HEIGHT,
        GLYPH_WIDTH,
    )
    glyph_data = b"".join(bitmap for _, bitmap in entries)
    unicode_data = b"".join(
        chr(codepoint).encode("utf-8") + b"\xff" for codepoint, _ in entries
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(header + glyph_data + unicode_data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("kernel/assets/font/unscii-16.hex"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("kernel/assets/font.psf"),
    )
    args = parser.parse_args()
    build(args.source, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
