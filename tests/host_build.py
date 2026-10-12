# SPDX-License-Identifier: GPL-3.0-or-later
"""Native host-only linker selection; target builds never import this module."""
import platform
import sys


def dead_strip_flags():
    return ["-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]


def native_uefi_flags():
    # Preserve the x86-64 Microsoft ABI fixture on Linux/Intel hosts. ARM
    # Darwin can test parsing with native callbacks, not that target ABI.
    return ["-DMANGROVE_HOST_TEST"] if (sys.platform == "darwin" and
        platform.machine().lower() in ("arm64", "aarch64")) else []
