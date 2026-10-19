#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run sanitizer-backed host regressions against production slab.c."""
from pathlib import Path
import os
import argparse
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEMP_ROOT = ROOT / "build/stage4.5-host-tmp"
TEMP_ROOT.mkdir(parents=True, exist_ok=True)
arguments = list(sys.argv[1:])
CC = arguments.pop(0) if arguments and not arguments[0].startswith("--") \
    else "cc"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--empty-reserve", action="store_true")
parser.add_argument("--cache-capacity", type=int, default=2)
parser.add_argument("--refill-batch", type=int, default=2)
args = parser.parse_args(arguments)
EMPTY_RESERVE = args.empty_reserve
if not 1 <= args.cache_capacity <= 16 or not 1 <= args.refill_batch <= 16:
    parser.error("cache capacity and refill batch must be between 1 and 16")

with tempfile.TemporaryDirectory(prefix="mangrove-slab-",
                                 dir=TEMP_ROOT) as directory:
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
    modes = (1,) if EMPTY_RESERVE else (0, 1)
    for deep_validation in modes:
        binary = Path(directory) / f"slab-test-deep-{deep_validation}"
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
            f"-DSLAB_DEEP_VALIDATION={deep_validation}",
            f"-DSLAB_LOCAL_CACHE_CAPACITY={args.cache_capacity}",
            f"-DSLAB_LOCAL_REFILL_BATCH={args.refill_batch}",
            "-Ikernel/include",
            "-Iinclude",
            "-idirafter",
            "libc/include",
        ]
        if EMPTY_RESERVE:
            command.append("-DSLAB_EMPTY_RESERVE_PER_CLASS=1")
        command.extend(["tests/slab_test.c", "-o", str(binary)])
        subprocess.run(command, cwd=ROOT, check=True)
        if EMPTY_RESERVE:
            subprocess.run([str(binary), "--empty-reserve"], cwd=ROOT,
                           env=environment, check=True)
        else:
            subprocess.run([str(binary)], cwd=ROOT, env=environment,
                           check=True)
            subprocess.run([str(binary), "--classifier-boundaries"],
                           cwd=ROOT, env=environment, check=True)
            subprocess.run([str(binary), "--classifier-concurrency"],
                           cwd=ROOT, env=environment, check=True)
            subprocess.run([str(binary), "--cpu-cache"],
                           cwd=ROOT, env=environment, check=True)
        print(f"slab host tests passed with deep validation={deep_validation}, "
              f"cache capacity={args.cache_capacity}, "
              f"refill batch={args.refill_batch}")
