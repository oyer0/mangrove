#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regress measured Pith frames that threatened guarded-stack safety."""
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: kernel_stack_usage_test.py KERNEL_BUILD_DIR", file=sys.stderr)
        return 2
    roots = [Path(sys.argv[1])]
    reports = sorted(path for root in roots for path in root.rglob("*.su"))
    if not reports:
        print("missing GCC stack-usage reports", file=sys.stderr)
        return 1

    frames: dict[str, list[int]] = {}
    oversized: list[tuple[int, str]] = []
    for report in reports:
        for line in report.read_text().splitlines():
            fields = line.split("\t")
            if len(fields) < 3:
                continue
            location, amount, _ = fields[:3]
            try:
                size = int(amount)
            except ValueError:
                continue
            if size > 64 * 1024:
                oversized.append((size, location))
            for function in ("account_purge_directory", "http_parser_init"):
                if function in location:
                    frames.setdefault(function, []).append(size)

    failed = False
    if oversized:
        failed = True
        for size, location in sorted(oversized, reverse=True):
            print(f"frame exceeds ordinary 64 KiB stack: {size} bytes: {location}",
                  file=sys.stderr)
    for function, limit in (("account_purge_directory", 1024),
                            ("http_parser_init", 1024)):
        observed = frames.get(function, [])
        if len(observed) != 1 or observed[0] > limit:
            failed = True
            print(f"{function}: expected one frame <= {limit} bytes, "
                  f"observed {observed}", file=sys.stderr)
        else:
            print(f"{function}: {observed[0]} byte frame (limit {limit})")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
