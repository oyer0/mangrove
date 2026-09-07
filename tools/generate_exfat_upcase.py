#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate Mangrove's exFAT recommended up-case include.

The input is a transcription of Table 25 (compressed format) from the
Microsoft exFAT File System Specification.  It deliberately does not import
an implementation's header or Unicode database.
"""

from pathlib import Path
import re
import sys


SOURCE = Path(__file__).with_name("exfat_upcase_table.txt")
OUTPUT = Path(__file__).parents[1] / "kernel/src/storage/exfat_upcase.inc"
EXPECTED_BYTES = 5836
EXPECTED_CHECKSUM = 0xE619D30D


def read_words() -> list[int]:
    text = SOURCE.read_text(encoding="ascii")
    words: list[int] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0]
        for token in line.split():
            if not re.fullmatch(r"[0-9A-Fa-f]{4}h", token):
                raise ValueError(f"{SOURCE}:{line_number}: invalid Table 25 word")
            value = int(token[:-1], 16)
            words.append(value)
    if len(words) != EXPECTED_BYTES // 2:
        raise ValueError(f"expected {EXPECTED_BYTES // 2} words, got {len(words)}")
    return words


def checksum(data: bytes) -> int:
    value = 0
    for byte in data:
        value = ((value >> 1) | ((value & 1) << 31)) & 0xFFFFFFFF
        value = (value + byte) & 0xFFFFFFFF
    return value


def render(data: bytes) -> str:
    lines = [
        "/* GENERATED FILE - DO NOT EDIT.",
        " * Source: Microsoft exFAT File System Specification, section 7.2.5.1, Table 25.",
        " * Generator: tools/generate_exfat_upcase.py.",
        " */",
    ]
    for offset in range(0, len(data), 16):
        values = ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16])
        lines.append(values + ",")
    return "\n".join(lines) + "\n"


def main() -> int:
    try:
        words = read_words()
        data = b"".join(word.to_bytes(2, "little") for word in words)
        actual = checksum(data)
        if len(data) != EXPECTED_BYTES or actual != EXPECTED_CHECKSUM:
            raise ValueError(
                f"Table 25 validation failed: {len(data)} bytes, checksum 0x{actual:08X}"
            )
        if "--check" in sys.argv[1:]:
            if not OUTPUT.exists() or OUTPUT.read_text(encoding="ascii") != render(data):
                print(f"{OUTPUT} is not up to date", file=sys.stderr)
                return 1
            print(f"verified {len(data)} bytes, checksum 0x{actual:08X}")
            return 0
        OUTPUT.write_text(render(data), encoding="ascii", newline="\n")
        print(f"generated {OUTPUT}: {len(data)} bytes, checksum 0x{actual:08X}")
        return 0
    except (OSError, ValueError) as error:
        print(f"exFAT up-case generation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
