#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and run the production MGFS staging/publication host fixture."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
with tempfile.TemporaryDirectory(prefix="mangrove-mgfs-stage-") as directory:
    binary = Path(directory) / "stage-test"
    subprocess.run([
        compiler, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-Wl,--gc-sections",
        "-I" + str(root / "kernel/include"),
        "-I" + str(root / "include"),
        "-idirafter", str(root / "libc/include"),
        str(root / "tests/mgfs_stage_cancel_test.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
