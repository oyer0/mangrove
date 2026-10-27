#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarize GCC -fstack-usage files from a target Pith build."""

from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: kernel_stack_usage_report.py DIRECTORY...", file=sys.stderr)
        return 2

    files = sorted({path for root in map(Path, sys.argv[1:])
                    for path in root.rglob("*.su")})
    if not files:
        print("no .su files found; build Pith with KERNEL_STACK_USAGE=1",
              file=sys.stderr)
        return 1

    frames: list[tuple[int, str, str]] = []
    dynamic: list[tuple[str, str]] = []
    for path in files:
        for line_number, line in enumerate(path.read_text().splitlines(), 1):
            fields = line.split("\t")
            if len(fields) < 3:
                continue
            location, amount, qualification = fields[:3]
            try:
                size = int(amount)
            except ValueError:
                dynamic.append((location, f"{amount} {qualification}"))
                continue
            frames.append((size, location, qualification))

    frames.sort(reverse=True)
    print(f"GCC target stack usage: {len(files)} object reports, "
          f"{len(frames)} bounded frame entries")
    print("Largest individual frames (bytes; not transitive call-chain totals):")
    for size, location, qualification in frames[:40]:
        print(f"{size:6d}  {qualification:10s}  {location}")
    if dynamic:
        print("Frames with non-constant or otherwise unparsed size:")
        for location, detail in dynamic:
            print(f"  {detail}: {location}")
    print("These are compiler frame estimates. They do not include interrupt "
          "nesting, assembly frames, or an inferred whole-call-chain bound.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
