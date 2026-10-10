#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compatibility entrypoint; production harnesses live in subsystem tests."""
from pathlib import Path
import subprocess
import sys
root = Path(__file__).resolve().parent
for name in ['vmm', 'heap', 'mgfs_write', 'history_fs', 'history']:
    subprocess.run([sys.executable, str(root / (name + '_test.py')), *sys.argv[1:]], check=True)
