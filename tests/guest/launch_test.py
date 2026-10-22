#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compatibility entrypoint for the focused launch prerequisites."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
RUNNERS = (
    ROOT / "tests/memory/vmm_test.py",
    ROOT / "tests/memory/heap_test.py",
    ROOT / "tests/filesystems/mgfs/mgfs_write_test.py",
    ROOT / "tests/filesystems/mgfs/history_fs_test.py",
    ROOT / "tests/userspace/history_test.py",
)
for runner in RUNNERS:
    subprocess.run([sys.executable, str(runner), *sys.argv[1:]], check=True)
