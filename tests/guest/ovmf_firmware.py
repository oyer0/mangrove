#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Discover the installed OVMF code and variable-store firmware files."""

import argparse
import os
from pathlib import Path
import platform
import subprocess


def candidate_paths(kind, *, system=None, share_root=None, qemu_prefix=None):
    if kind not in ("code", "vars"):
        raise ValueError("kind must be 'code' or 'vars'")

    system = system or platform.system()
    share_root = Path(share_root or "/usr/share")
    if system == "Linux":
        names = {
            "code": (
                "edk2/x64/OVMF_CODE.4m.fd",
                "OVMF/OVMF_CODE_4M.fd",
            ),
            "vars": (
                "edk2/x64/OVMF_VARS.4m.fd",
                "OVMF/OVMF_VARS_4M.fd",
            ),
        }
        return [share_root / name for name in names[kind]]

    if system == "Darwin":
        prefix = qemu_prefix or os.environ.get("QEMU_PREFIX", "")
        if not prefix:
            try:
                prefix = subprocess.check_output(
                    ["brew", "--prefix", "qemu"],
                    text=True, stderr=subprocess.DEVNULL,
                ).strip()
            except (FileNotFoundError, subprocess.CalledProcessError):
                prefix = ""
        if not prefix:
            return []
        name = "edk2-x86_64-code.fd" if kind == "code" else "edk2-i386-vars.fd"
        return [Path(prefix) / "share/qemu" / name]

    return []


def default_path(kind, *, system=None, share_root=None, qemu_prefix=None,
                 environ=None):
    environ = os.environ if environ is None else environ
    variable_names = (("OVMF_CODE_SOURCE", "OVMF_CODE") if kind == "code"
                      else ("OVMF_VARS_SOURCE", "OVMF_VARS"))
    for variable in variable_names:
        value = environ.get(variable)
        if value:
            return Path(value)

    candidates = candidate_paths(
        kind, system=system, share_root=share_root, qemu_prefix=qemu_prefix)
    if not candidates:
        return Path("OVMF_CODE_SOURCE" if kind == "code" else "OVMF_VARS_SOURCE")
    return next((path for path in candidates if path.is_file()), candidates[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("code", "vars"), required=True)
    parser.add_argument("--qemu-prefix")
    args = parser.parse_args()
    print(default_path(args.kind, qemu_prefix=args.qemu_prefix))


if __name__ == "__main__":
    main()
