#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and run the production MGFS staging/publication host fixture."""
from pathlib import Path
import subprocess
import sys
import tempfile
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[2] / "shared"))
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[3]
compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
with tempfile.TemporaryDirectory(prefix="mangrove-mgfs-stage-") as directory:
    binary = Path(directory) / "stage-test"
    subprocess.run([
        compiler, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", *dead_strip_flags(),
        "-I" + str(root / "kernel/include"),
        "-I" + str(root / "include"),
        "-idirafter", str(root / "libc/include"),
        str(root / "tests/filesystems/mgfs/mgfs_stage_cancel_test.c"),
        str(root / "libc/src/utf8.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
