#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify OVMF discovery across the supported Linux package layouts."""

from pathlib import Path
import tempfile

import ovmf_firmware


def write_pair(directory, code_name, vars_name):
    code = directory / code_name
    variables = directory / vars_name
    code.parent.mkdir(parents=True, exist_ok=True)
    variables.parent.mkdir(parents=True, exist_ok=True)
    code.write_bytes(b"code")
    variables.write_bytes(b"vars")
    return code, variables


def main():
    with tempfile.TemporaryDirectory(prefix="ovmf-layout-") as scratch:
        share = Path(scratch)
        arch_code, arch_vars = write_pair(
            share, "edk2/x64/OVMF_CODE.4m.fd",
            "edk2/x64/OVMF_VARS.4m.fd")
        debian_code, debian_vars = write_pair(
            share, "OVMF/OVMF_CODE_4M.fd", "OVMF/OVMF_VARS_4M.fd")

        assert ovmf_firmware.default_path(
            "code", system="Linux", share_root=share) == arch_code
        assert ovmf_firmware.default_path(
            "vars", system="Linux", share_root=share) == arch_vars

        arch_code.unlink()
        arch_vars.unlink()
        assert ovmf_firmware.default_path(
            "code", system="Linux", share_root=share) == debian_code
        assert ovmf_firmware.default_path(
            "vars", system="Linux", share_root=share) == debian_vars

        override_code = share / "custom/code.fd"
        override_vars = share / "custom/vars.fd"
        override_code.parent.mkdir(parents=True)
        override_vars.write_bytes(b"vars")
        environment = {
            "OVMF_CODE_SOURCE": str(override_code),
            "OVMF_VARS_SOURCE": str(override_vars),
        }
        assert ovmf_firmware.default_path(
            "code", system="Linux", share_root=share,
            environ=environment) == override_code
        assert ovmf_firmware.default_path(
            "vars", system="Linux", share_root=share,
            environ=environment) == override_vars

    print("OVMF firmware discovery passed (Arch, Debian, explicit overrides)")


if __name__ == "__main__":
    main()
