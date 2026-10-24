#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production formatter, checker, updater, and image admission."""

import pathlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[2] / "shared"))
from host_build import dead_strip_flags, native_uefi_flags

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import populate_mgfs  # noqa: E402
import update_mgfs  # noqa: E402
from image_payloads import DATA_MANIFEST, PAYLOAD_MANIFEST  # noqa: E402


def run(*args):
    return subprocess.run(args, capture_output=True, text=True)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: mgfs_version_test.py <mkmgfs> <mgfsck> <host-cc>")
    mkmgfs, mgfsck = map(pathlib.Path, sys.argv[1:3])
    compiler = sys.argv[3]
    checker = ROOT / "tools/check_mgfs_version.py"
    with tempfile.TemporaryDirectory(prefix="mangrove-mgfs-version-") as temp:
        boot_test = pathlib.Path(temp) / "boot-version-test"
        subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-variable", *native_uefi_flags(),
                        "-ffunction-sections", "-fdata-sections",
                        *dead_strip_flags(), "-I" + str(ROOT / "boot/include"),
                        "-I" + str(ROOT / "include"),
                        str(ROOT / "tests/filesystems/mgfs/mgfs_boot_version_test.c"),
                        str(ROOT / "boot/src/mgfs.c"), "-o", str(boot_test)],
                       check=True)
        current = pathlib.Path(temp) / "current.img"
        subprocess.run([str(mkmgfs), "--blocks", "3200", "--uuid",
                        "11111111-2222-3333-4444-555555555555",
                        "--format-time-ns", "1", str(current)], check=True)
        original = current.read_bytes()
        assert original[:8] == b"MGFSv1\0\0"
        assert struct.unpack_from("<QQ", original, 8) == (1, 2)
        assert run(str(mgfsck), str(current)).returncode == 0
        assert run(sys.executable, str(checker), str(current)).returncode == 0
        dummy = pathlib.Path(temp) / "payload"
        dummy.write_bytes(b"fixture")
        payloads = [str(dummy)] * (len(populate_mgfs.PAYLOAD_MANIFEST) +
                                   len(populate_mgfs.DATA_MANIFEST))

        legacy_one = None
        for minor in (0, 1):
            legacy = pathlib.Path(temp) / ("minor-%d.img" % minor)
            image = bytearray(original)
            struct.pack_into("<Q", image, 16, minor)
            superblock = bytearray(image[:populate_mgfs.B])
            populate_mgfs.checksum(superblock, 192, 200)
            image[:populate_mgfs.B] = superblock
            legacy.write_bytes(image)
            before = legacy.read_bytes()
            assert run(str(mgfsck), str(legacy)).returncode != 0
            check = run(sys.executable, str(checker), str(legacy))
            assert check.returncode != 0 and "image preserved" in check.stderr
            populate = run(sys.executable, str(ROOT / "tools/populate_mgfs.py"),
                           str(legacy), *payloads)
            assert populate.returncode != 0 and "1.2 required" in populate.stderr
            try:
                update_mgfs.update(str(legacy), [], [])
            except RuntimeError as error:
                assert "requires 1.2" in str(error)
            else:
                raise AssertionError("updater accepted unsupported minor")
            assert legacy.read_bytes() == before
            if minor == 1:
                legacy_one = legacy

        # Fresh population and the incremental updater must both install the
        # fullscreen readers and the system-owned manual directory.
        installed = pathlib.Path(temp) / "installed.img"
        installed.write_bytes(original)
        payload_paths = [str(dummy)] * len(PAYLOAD_MANIFEST)
        data_paths = [str(dummy)] * len(DATA_MANIFEST)
        populate_result = run(
            sys.executable, str(ROOT / "tools/populate_mgfs.py"),
            str(installed), *payload_paths, *data_paths)
        assert populate_result.returncode == 0, populate_result.stderr
        update_mgfs.update(str(installed), payload_paths, data_paths)

        installed_image = installed.read_bytes()
        layout = {
            "record_table_start": struct.unpack_from("<Q", installed_image,
                                                       136)[0],
            "record_count": struct.unpack_from("<Q", installed_image,
                                                 152)[0],
        }

        def record(record_id):
            offset = update_mgfs.record_offset(
                installed_image, layout["record_table_start"], record_id,
                layout["record_count"])
            return installed_image[offset:offset + update_mgfs.RECORD_BYTES]

        def directory(record_id):
            return update_mgfs.record_payload(installed_image,
                                                record(record_id))

        root_id = update_mgfs.directory_find(directory(1), "share")
        assert root_id is not None
        share_id = update_mgfs.directory_find(directory(root_id), "info")
        assert share_id is not None
        assert struct.unpack_from("<Q", record(share_id))[0] == 2
        bin_id = update_mgfs.directory_find(directory(1), "bin")
        assert bin_id is not None
        assert update_mgfs.directory_find(directory(bin_id), "view") is not None
        assert update_mgfs.directory_find(directory(bin_id), "info") is not None
        help_id = update_mgfs.directory_find(directory(root_id), "help")
        assert help_id is not None
        assert update_mgfs.directory_find(directory(help_id), "view") is not None
        assert update_mgfs.directory_find(directory(help_id), "info") is not None

        subprocess.run([str(boot_test), str(current), str(legacy_one)], check=True)
        assert current.read_bytes() == original
    print("MGFS 1.2 formatter/checker accepted; old minors rejected without media changes")


if __name__ == "__main__":
    main()
