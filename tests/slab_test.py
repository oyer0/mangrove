#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run sanitizer-backed host regressions against production slab.c."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
EMPTY_RESERVE = "--empty-reserve" in sys.argv[2:]

with tempfile.TemporaryDirectory(prefix="mangrove-slab-") as directory:
    binary = Path(directory) / "slab-test"
    command = [
        CC,
        "-std=gnu11",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pthread",
        "-fsanitize=address,undefined",
        "-DSLAB_CORE_DIAGNOSTICS=1",
        "-Ikernel/include",
        "-Iinclude",
        "-idirafter",
        "libc/include",
    ]
    if EMPTY_RESERVE:
        command.append("-DSLAB_EMPTY_RESERVE_PER_CLASS=1")
    command.extend(["tests/slab_test.c", "-o", str(binary)])
    subprocess.run(command, cwd=ROOT, check=True)
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
    if EMPTY_RESERVE:
        subprocess.run([str(binary), "--empty-reserve"], cwd=ROOT,
                       env=environment, check=True)
    else:
        subprocess.run([str(binary)], cwd=ROOT, env=environment, check=True)
        subprocess.run([str(binary), "--classifier-boundaries"], cwd=ROOT,
                       env=environment, check=True)
        subprocess.run([str(binary), "--classifier-concurrency"], cwd=ROOT,
                       env=environment, check=True)
