#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and run production FAT32/exFAT mount-identity fixtures."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[1]
compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
subprocess.run([sys.executable, str(root / "tools/generate_fat32_uppercase.py"),
                "--check"], check=True)
with tempfile.TemporaryDirectory(prefix="mangrove-fat-identity-") as directory:
    for name in ("fat32_mount_identity_test", "fat32_namespace_test",
                 "exfat_mount_identity_test"):
        binary = Path(directory) / name
        subprocess.run([
            compiler, "-O1", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-type-limits", "-ffunction-sections",
            "-fdata-sections", "-fsanitize=address,undefined",
            *dead_strip_flags(), "-I" + str(root / "kernel/include"),
            "-I" + str(root / "include"), "-idirafter",
            str(root / "libc/include"),
            str(root / "tests" / (name + ".c")), "-o", str(binary),
        ], check=True)
        args = [str(binary)]
        if name == "fat32_namespace_test" and os.environ.get("FAT32_NAMESPACE_IMAGE"):
            args.append(os.environ["FAT32_NAMESPACE_IMAGE"])
        if name == "fat32_mount_identity_test" and os.environ.get("FAT32_IMAGE"):
            args.append(os.environ["FAT32_IMAGE"])
            if os.environ.get("FAT32_REPLACE_IMAGE"):
                args.append(os.environ["FAT32_REPLACE_IMAGE"])
        if name == "exfat_mount_identity_test" and os.environ.get("EXFAT_GROWTH_IMAGE"):
            args.append(os.environ["EXFAT_GROWTH_IMAGE"])
            if os.environ.get("EXFAT_NONROOT_GROWTH_IMAGE"):
                args.append(os.environ["EXFAT_NONROOT_GROWTH_IMAGE"])
        subprocess.run(args, check=True)
        if name == "exfat_mount_identity_test" and os.environ.get(
                "EXFAT_ORDINARY_SPLIT_IMAGE"):
            subprocess.run([str(binary), "--ordinary-split-image",
                            os.environ["EXFAT_ORDINARY_SPLIT_IMAGE"]],
                           check=True)
        if name == "exfat_mount_identity_test" and os.environ.get(
                "EXFAT_REPLACE_SPLIT_IMAGE"):
            subprocess.run([str(binary), "--ordinary-split-image",
                            os.environ["EXFAT_REPLACE_SPLIT_IMAGE"]],
                           check=True)
            subprocess.run([str(binary), "--replace-split-image",
                            os.environ["EXFAT_REPLACE_SPLIT_IMAGE"]],
                           check=True)
        if name == "exfat_mount_identity_test":
            for kind in ("file", "directory"):
                image = os.environ.get("EXFAT_RETIRE_" + kind.upper() + "_IMAGE")
                if image:
                    subprocess.run([str(binary), "--seed-retire-" + kind + "-image",
                                    image], check=True)
                    subprocess.run([str(binary), "--retire-" + kind + "-image",
                                    image], check=True)
        if name == "fat32_mount_identity_test" and os.environ.get("FAT32_GROWTH_IMAGE"):
            subprocess.run([str(binary), "--growth-image",
                            os.environ["FAT32_GROWTH_IMAGE"]], check=True)
        if name == "fat32_mount_identity_test" and os.environ.get("FAT32_RETIRE_IMAGE"):
            subprocess.run([str(binary), "--retire-image",
                            os.environ["FAT32_RETIRE_IMAGE"]], check=True)
        if name == "fat32_mount_identity_test" and os.environ.get("FAT32_CROSS_CREATE_IMAGE"):
            subprocess.run([str(binary), "--cross-create-image",
                            os.environ["FAT32_CROSS_CREATE_IMAGE"]], check=True)
        if name == "fat32_mount_identity_test" and os.environ.get("FAT32_CROSS_DELETE_IMAGE"):
            subprocess.run([str(binary), "--cross-delete-image",
                            os.environ["FAT32_CROSS_DELETE_IMAGE"]], check=True)
        if name == "fat32_mount_identity_test" and os.environ.get("FAT32_RMDIR_IMAGE"):
            subprocess.run([str(binary), "--retire-directory-image",
                            os.environ["FAT32_RMDIR_IMAGE"]], check=True)
