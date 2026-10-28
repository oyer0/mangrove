#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify QEMU RTC trust selection and incremental kernel rebuilds."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def make(*arguments, timeout=240):
    result = subprocess.run(
        ["make", "--no-print-directory", *map(str, arguments)],
        cwd=ROOT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout,
    )
    if result.returncode:
        raise AssertionError(
            f"make {' '.join(map(str, arguments))} failed:\n"
            f"{result.stdout[-12000:]}"
        )
    return result.stdout


def main_compile_line(output):
    return next((line for line in output.splitlines()
                 if "kernel/src/main.c" in line and " -c " in line), None)


run_plan = make("-n", "-B", "run")
run_line = main_compile_line(run_plan)
assert run_line and "-DRTC_UTC_TRUST=1" in run_line, run_line
assert "-rtc base=utc,clock=host" in run_plan, run_plan[-5000:]
deterministic_plan = make("-n", "QEMU_RTC_CLOCK=vm", "run")
assert "-rtc base=utc,clock=vm" in deterministic_plan, deterministic_plan[-5000:]

default_plan = make("-n", "-B", "binaries")
default_line = main_compile_line(default_plan)
assert default_line and "-DRTC_UTC_TRUST=1" not in default_line, default_line

flash_plan = make("-n", "-B", "flash-image")
flash_line = main_compile_line(flash_plan)
assert flash_line and "-DRTC_UTC_TRUST=1" not in flash_line, flash_line

with tempfile.TemporaryDirectory(prefix="mangrove-rtc-build-policy-") as tmp:
    build_dir = Path(tmp) / "build"
    kernel = build_dir / "Mangrove" / "pith.elf"
    stamp = build_dir / "kernel" / ".rtc-utc-trust"

    untrusted_first = make("-j4", f"BUILD_DIR={build_dir}",
                           "RTC_UTC_TRUST=0", kernel)
    first_line = main_compile_line(untrusted_first)
    assert first_line and "-DRTC_UTC_TRUST=1" not in first_line, first_line
    assert stamp.read_text().strip() == "0"
    assert kernel.is_file()

    unchanged = make("-j4", f"BUILD_DIR={build_dir}",
                     "RTC_UTC_TRUST=0", kernel)
    assert main_compile_line(unchanged) is None, unchanged[-2000:]

    trusted_switch = make("-j4", f"BUILD_DIR={build_dir}",
                          "RTC_UTC_TRUST=1", kernel)
    trusted_line = main_compile_line(trusted_switch)
    assert trusted_line and "-DRTC_UTC_TRUST=1" in trusted_line, trusted_line
    assert stamp.read_text().strip() == "1"

    untrusted_switch = make("-j4", f"BUILD_DIR={build_dir}",
                            "RTC_UTC_TRUST=0", kernel)
    untrusted_line = main_compile_line(untrusted_switch)
    assert untrusted_line and "-DRTC_UTC_TRUST=1" not in untrusted_line, untrusted_line
    assert stamp.read_text().strip() == "0"

print("RTC build policy: QEMU trust selection and incremental mode changes passed")
