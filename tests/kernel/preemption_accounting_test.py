#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check production counter call sites and stress the shared atomic helper."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"

timer = (ROOT / "kernel/src/timer.c").read_text()
scheduler = (ROOT / "kernel/src/scheduler.c").read_text()
assert timer.count("preemption_count_increment(&preemptions);") == 2
assert "return preemption_count_snapshot(&preemptions);" in timer
assert scheduler.count(
    "preemption_count_increment(&scheduler_stats.timer_preemptions);") == 2
assert "preemption_count_snapshot(\n            &scheduler_stats.timer_preemptions)" in scheduler
assert "preemption_count_snapshot(&scheduler_stats.timer_preemptions)" in scheduler
assert "*stats = scheduler_stats;" not in scheduler

with tempfile.TemporaryDirectory(
        prefix="mangrove-preemption-accounting-",
        dir=os.environ.get("TMPDIR") or None) as temporary:
    binary = Path(temporary) / "test"
    subprocess.run(
        [CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
         "-fsanitize=address,undefined", "-Ikernel/include", "-Iinclude",
         "tests/kernel/preemption_accounting_test.c", "-o", str(binary)],
        cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
                   check=True)
