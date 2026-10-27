#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the generated production UNSCII-16 PSF2 asset against its source."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

MAGIC = b"\x72\xb5\x4a\x86"
REQUIRED = set(range(0x20, 0x7f)) | {
    0x00e9, 0x010d, 0x015f, 0x0301, 0x030c, 0x0327,
    0x2500, 0x2502, 0x250c, 0x2518, 0x2588, 0x2591,
}


def source_entries(path):
    entries = {}
    for line_number, raw_line in enumerate(path.read_text("ascii").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        codepoint_text, bitmap_text = line.split(":", 1)
        if len(bitmap_text) != 32:
            continue
        codepoint = int(codepoint_text, 16)
        if codepoint in entries:
            raise AssertionError(
                f"duplicate source mapping U+{codepoint:04X} at {line_number}")
        entries[codepoint] = bytes.fromhex(bitmap_text)
    return entries


def decode_asset(data):
    if len(data) < 32:
        raise AssertionError("PSF2 asset is shorter than its header")
    (magic, version, header_size, flags, glyph_count, bytes_per_glyph,
     height, width) = struct.unpack_from("<4s7I", data)
    assert magic == MAGIC, "incorrect PSF2 magic"
    assert version == 0, f"unsupported PSF2 version {version}"
    assert header_size == 32, f"unexpected PSF2 header size {header_size}"
    assert flags == 1, f"expected one Unicode table, got flags {flags:#x}"
    assert (width, height, bytes_per_glyph) == (8, 16, 16), \
        f"unexpected glyph geometry: {width}x{height}, {bytes_per_glyph} bytes"
    assert glyph_count == 2997, f"unexpected source glyph count {glyph_count}"

    glyph_start = header_size
    glyph_end = glyph_start + glyph_count * bytes_per_glyph
    assert glyph_end <= len(data), "truncated PSF2 glyph table"
    glyphs = [data[glyph_start + i * bytes_per_glyph:
                   glyph_start + (i + 1) * bytes_per_glyph]
              for i in range(glyph_count)]

    offset = glyph_end
    by_codepoint = {}
    for glyph_index in range(glyph_count):
        mapped = 0
        while True:
            if offset >= len(data):
                raise AssertionError(
                    f"unterminated Unicode entry for glyph {glyph_index}")
            if data[offset] == 0xff:
                offset += 1
                break
            if data[offset] == 0xfe:
                raise AssertionError(
                    "unexpected sequence mapping in scalar-only font asset")
            lead = data[offset]
            if lead <= 0x7f:
                scalar_bytes = data[offset:offset + 1]
            elif 0xc2 <= lead <= 0xdf:
                scalar_bytes = data[offset:offset + 2]
            elif 0xe0 <= lead <= 0xef:
                scalar_bytes = data[offset:offset + 3]
            elif 0xf0 <= lead <= 0xf4:
                scalar_bytes = data[offset:offset + 4]
            else:
                raise AssertionError(
                    f"invalid UTF-8 lead byte {lead:#x} at offset {offset}")
            try:
                text = scalar_bytes.decode("utf-8", errors="strict")
            except UnicodeDecodeError as error:
                raise AssertionError(
                    f"malformed UTF-8 mapping at offset {offset}") from error
            if len(text) != 1:
                raise AssertionError("mapping is not one Unicode scalar")
            codepoint = ord(text)
            if codepoint in by_codepoint:
                raise AssertionError(f"duplicate PSF2 mapping U+{codepoint:04X}")
            by_codepoint[codepoint] = glyphs[glyph_index]
            offset += len(scalar_bytes)
            mapped += 1
        assert mapped == 1, \
            f"glyph {glyph_index} has {mapped} scalar mappings, expected one"

    assert offset == len(data), f"unexpected {len(data) - offset} trailing PSF2 bytes"
    assert len(by_codepoint) == glyph_count, "mapping count differs from glyph count"
    missing = REQUIRED - set(by_codepoint)
    assert not missing, "missing required mappings: " + ", ".join(
        f"U+{value:04X}" for value in sorted(missing))
    assert 0x4e00 not in by_codepoint, "control sample U+4E00 unexpectedly mapped"
    return by_codepoint


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--converter", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = source_entries(root / args.source)
    actual = (root / args.asset).read_bytes()
    mappings = decode_asset(actual)

    with tempfile.TemporaryDirectory(prefix="mangrove-font-asset-") as directory:
        regenerated = Path(directory) / "font.psf"
        subprocess.run([
            sys.executable, str(args.converter), "--source", str(args.source),
            "--output", str(regenerated),
        ], cwd=root, check=True)
        assert regenerated.read_bytes() == actual, \
            "checked-in PSF2 asset differs from converter output"

    assert set(source) == set(mappings), "source and PSF2 Unicode mappings differ"
    for codepoint, bitmap in source.items():
        assert mappings[codepoint] == bitmap, \
            f"PSF2 glyph data differs from source for U+{codepoint:04X}"
    print(f"UNSCII-16 asset: 8x16, {len(mappings)} unique mappings; converter and source match")


if __name__ == "__main__":
    main()
