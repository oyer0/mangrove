#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and run production FAT32/exFAT mount-identity fixtures."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
with tempfile.TemporaryDirectory(prefix="mangrove-fat-identity-") as directory:
    for name in ("fat32_mount_identity_test", "exfat_mount_identity_test"):
        binary = Path(directory) / name
        subprocess.run([
            compiler, "-O1", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-type-limits", "-ffunction-sections",
            "-fdata-sections", "-fsanitize=address,undefined",
            "-Wl,--gc-sections", "-I" + str(root / "kernel/include"),
            "-I" + str(root / "include"), "-idirafter",
            str(root / "libc/include"),
            str(root / "tests" / (name + ".c")), "-o", str(binary),
        ], check=True)
        args = [str(binary)]
        if name == "fat32_mount_identity_test" and os.environ.get("FAT32_IMAGE"):
            args.append(os.environ["FAT32_IMAGE"])
        subprocess.run(args, check=True)
