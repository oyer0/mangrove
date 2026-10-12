#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate bounded FAT32 simple-uppercase ranges from pinned Unicode data.

The checked-in pairs are UnicodeData.txt field 12, not Python's full upper()
or casefold(). --unicode-data additionally verifies their original provenance.
No network access is needed for normal generation or --check.
"""
import argparse
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools/fat32_uppercase_15_1.txt"
OUTPUT = ROOT / "kernel/src/storage/fat32_uppercase.inc"
SOURCE_SHA256 = "32b84c628492f6290510675ec07894bca579821f8a386ef75111b8b92b87f914"
UNICODE_SHA256 = "2fc713e6a31a87c4850a37fe2caffa4218180fadb5de86b43a143ddb4581fb86"


def read_pairs():
    data = SOURCE.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("pinned simple-uppercase input checksum mismatch")
    pairs = [tuple(int(v, 16) for v in line.split(";"))
             for line in data.decode("ascii").splitlines()
             if line and not line.startswith("#")]
    assert len(pairs) == 1450 and pairs == sorted(set(pairs))
    mapping = dict(pairs)
    assert len(mapping) == len(pairs)
    for source, target in pairs:
        assert source != target
        assert all(0 <= v <= 0x10FFFF and not 0xD800 <= v <= 0xDFFF
                   for v in (source, target))
        assert mapping.get(target, target) == target
    return pairs


def ranges(pairs):
    result = []
    index = 0
    while index < len(pairs):
        first, target = pairs[index]
        last, delta, stride = first, target - first, 1
        if index + 1 < len(pairs):
            next_source, next_target = pairs[index + 1]
            if next_target - next_source == delta and next_source - first in (1, 2):
                stride = next_source - first
                while index + 1 < len(pairs):
                    source, target = pairs[index + 1]
                    if source - last != stride or target - source != delta:
                        break
                    index += 1
                    last = source
        result.append((first, last, delta, stride))
        index += 1
    expanded = [(source, source + delta) for first, last, delta, stride in result
                for source in range(first, last + 1, stride)]
    assert expanded == pairs
    assert all(a[1] < b[0] for a, b in zip(result, result[1:]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--unicode-data", type=Path)
    args = parser.parse_args()
    pairs = read_pairs()
    if args.unicode_data:
        data = args.unicode_data.read_bytes()
        if hashlib.sha256(data).hexdigest() != UNICODE_SHA256:
            raise ValueError("expected the pinned Unicode 15.1.0 UnicodeData.txt")
        original = []
        for line in data.decode("utf-8").splitlines():
            fields = line.split(";")
            if fields[12]:
                original.append((int(fields[0], 16), int(fields[12], 16)))
        assert original == pairs
    grouped = ranges(pairs)
    text = ("/* SPDX-License-Identifier: Unicode-3.0\n"
            " * GENERATED: tools/generate_fat32_uppercase.py; Unicode 15.1.0.\n"
            " * Copyright Unicode, Inc. See documentation/filesystem/fat32/unicode-license.txt.\n"
            " */\n")
    text += "".join(f"{{0x{first:06x}U, 0x{last:06x}U, {delta}, {stride}U}},\n"
                    for first, last, delta, stride in grouped)
    if args.check:
        if OUTPUT.read_text(encoding="ascii") != text:
            raise ValueError("FAT32 uppercase include is not up to date")
    else:
        OUTPUT.write_text(text, encoding="ascii")
    print(f"FAT32 Unicode 15.1.0: {len(pairs)} mappings, {len(grouped)} ranges, "
          f"{len(grouped) * 16} target bytes")


if __name__ == "__main__":
    main()
