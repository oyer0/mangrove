#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Platform-aware QEMU accelerator defaults and KVM access preflight."""

import argparse
import os
import subprocess
import sys


def default_accel(platform_name=None):
    """Use KVM for Linux guest tests and retain TCG on other hosts."""
    host = sys.platform if platform_name is None else platform_name
    return "kvm" if host.startswith("linux") else "tcg"


def accel_error(accel, platform_name=None, device="/dev/kvm",
                open_device=os.open, close_device=os.close,
                qemu="qemu-system-x86_64", qemu_runner=subprocess.run):
    """Return an actionable error if the requested accelerator is unavailable."""
    host = sys.platform if platform_name is None else platform_name
    if accel == "tcg":
        return None
    if accel != "kvm":
        raise ValueError("unsupported QEMU accelerator: " + str(accel))
    if not host.startswith("linux"):
        return ("KVM acceleration is available only on Linux. No accelerator "
                "fallback was attempted; select TCG explicitly with "
                "--accel tcg or the target's *_ACCEL=tcg Make variable.")

    try:
        qemu_result = qemu_runner(
            [qemu, "-accel", "help"], capture_output=True, text=True,
            timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        return (f"KVM was requested, but QEMU accelerator discovery failed for "
                f"{qemu}: {error}. No TCG fallback was attempted. Install or "
                "select a QEMU binary with KVM support, or explicitly select "
                "TCG with --accel tcg or the target's *_ACCEL=tcg Make variable.")
    accelerators = (qemu_result.stdout + "\n" + qemu_result.stderr).split()
    if qemu_result.returncode != 0 or "kvm" not in accelerators:
        return (f"KVM was requested, but QEMU executable {qemu!r} does not "
                "advertise KVM acceleration (`qemu -accel help`). No TCG "
                "fallback was attempted. Install or select a QEMU binary with "
                "KVM support, or explicitly select TCG with --accel tcg or "
                "the target's *_ACCEL=tcg Make variable.")

    try:
        descriptor = open_device(device, os.O_RDWR | getattr(os, "O_CLOEXEC", 0))
    except OSError as error:
        detail = error.strerror or str(error)
        return (f"KVM was selected, but {device} cannot be opened read/write: "
                f"{detail}. No TCG fallback was attempted. On Linux, check "
                "that KVM is enabled and that this process or its sandbox can "
                "access /dev/kvm. If Codex's ordinary sandbox hides the device, "
                "use its approved elevated execution path; do not weaken host "
                "device permissions. To opt into emulation explicitly, pass "
                "--accel tcg or set the target's *_ACCEL=tcg Make variable "
                "(for MTS, MTS_ACCEL=tcg).")
    try:
        close_device(descriptor)
    except OSError as error:
        return f"KVM device preflight could not close {device}: {error}"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accel", choices=("kvm", "tcg"), required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    args = parser.parse_args()
    error = accel_error(args.accel, qemu=args.qemu)
    if error:
        parser.error(error)
    return 0


if __name__ == "__main__":
    sys.exit(main())
