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
        subprocess.run([str(mkmgfs), "--blocks", "3600", "--uuid",
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
        fresh_check = run(str(mgfsck), str(installed))
        assert fresh_check.returncode == 0, fresh_check.stderr

        def inspect_manuals(image_bytes):
            layout = {
                "record_table_start": struct.unpack_from("<Q", image_bytes,
                                                           136)[0],
                "record_count": struct.unpack_from("<Q", image_bytes,
                                                     152)[0],
            }

            def record(record_id):
                offset = update_mgfs.record_offset(
                    image_bytes, layout["record_table_start"], record_id,
                    layout["record_count"])
                return image_bytes[offset:offset + update_mgfs.RECORD_BYTES]

            def directory(record_id):
                return update_mgfs.record_payload(image_bytes,
                                                   record(record_id))

            share_id = update_mgfs.directory_find(directory(1), "share")
            assert share_id is not None
            info_id = update_mgfs.directory_find(directory(share_id), "info")
            assert info_id is not None
            assert struct.unpack_from("<Q", record(info_id))[0] == 2
            entries = {
                name: (record_id, flags)
                for _offset, _entry_size, record_id, flags, name
                in update_mgfs.directory_entries(directory(info_id))
            }
            manuals = {}
            for name in populate_mgfs.INFO_FILES:
                encoded = name.encode("utf-8")
                assert encoded in entries, "missing installed manual: " + name
                record_id, flags = entries[encoded]
                assert flags == 1
                manual_record = record(record_id)
                assert update_mgfs.record_owner(manual_record) == \
                    update_mgfs.VFS_UID_SYSTEM
                manual = update_mgfs.record_payload(image_bytes, manual_record)
                assert manual == (ROOT / populate_mgfs.INFO_SOURCE_DIR /
                                  name).read_bytes()
                manuals[name] = (record_id, manual)
            help_id = update_mgfs.directory_find(directory(share_id), "help")
            assert help_id is not None
            reed_help_id = update_mgfs.directory_find(directory(help_id), "reed")
            assert reed_help_id is not None
            reed_help_record = record(reed_help_id)
            reed_help = update_mgfs.record_payload(image_bytes,
                                                    reed_help_record)
            expected_help = (ROOT / "share/help/reed").read_bytes()
            assert reed_help == expected_help
            help_id = update_mgfs.directory_find(directory(share_id), "help")
            assert help_id is not None
            help_entries = {
                name: (record_id, flags)
                for _offset, _entry_size, record_id, flags, name
                in update_mgfs.directory_entries(directory(help_id))
            }
            help_records = {}
            for name in populate_mgfs.HELP_FILES:
                encoded = name.encode("utf-8")
                assert encoded in help_entries, "missing installed help: " + name
                record_id, flags = help_entries[encoded]
                assert flags == 1
                help_record = record(record_id)
                assert update_mgfs.record_owner(help_record) == \
                    update_mgfs.VFS_UID_SYSTEM
                payload = update_mgfs.record_payload(image_bytes, help_record)
                assert payload == (ROOT / "share/help" / name).read_bytes()
                help_records[name] = (record_id, payload)
            assert b"Alt+S selects all" in reed_help and b"Alt+Q" not in reed_help
            return manuals, help_records

        fresh_manuals, fresh_help = inspect_manuals(installed.read_bytes())

        # Put one prior quick-help payload into the disposable fresh image.
        # The incremental updater must replace it from share/help while
        # preserving the installed record identity.
        stale_image = bytearray(installed.read_bytes())
        stale_layout = {
            "allocation_bitmap_start": struct.unpack_from("<Q", stale_image, 104)[0],
            "allocation_bitmap_blocks": struct.unpack_from("<Q", stale_image, 112)[0],
            "record_bitmap_start": struct.unpack_from("<Q", stale_image, 120)[0],
            "record_bitmap_blocks": struct.unpack_from("<Q", stale_image, 128)[0],
            "record_table_start": struct.unpack_from("<Q", stale_image, 136)[0],
            "record_table_blocks": struct.unpack_from("<Q", stale_image, 144)[0],
            "record_count": struct.unpack_from("<Q", stale_image, 152)[0],
            "data_start": struct.unpack_from("<Q", stale_image, 160)[0],
            "data_blocks": struct.unpack_from("<Q", stale_image, 168)[0],
        }
        view_help_id = fresh_help["view"][0]
        help_table = update_mgfs.replace_file_payload(
            stale_image, stale_layout, view_help_id,
            b"name=view\ncategory=files\ndescription=old quick help\nusage=view FILE\n",
            set())
        update_mgfs.refresh_table_checksum(stale_image, stale_layout, help_table)
        update_mgfs.refresh_metadata_checksums(stale_image, stale_layout)
        installed.write_bytes(stale_image)
        assert run(str(mgfsck), str(installed)).returncode == 0

        update_mgfs.update(str(installed), payload_paths, data_paths)

        installed_image = installed.read_bytes()
        updated_manuals, updated_help = inspect_manuals(installed_image)
        assert {name: item[0] for name, item in fresh_manuals.items()} == \
            {name: item[0] for name, item in updated_manuals.items()}
        assert {name: item[0] for name, item in fresh_help.items()} == \
            {name: item[0] for name, item in updated_help.items()}
        assert {name: item[1] for name, item in fresh_help.items()} == \
            {name: item[1] for name, item in updated_help.items()}

        # Model an image from the previous reader release, which had the
        # system-owned /share/info directory but no installed manual files.
        # The incremental updater must fill that directory without changing
        # its identity or leaving the filesystem inconsistent.
        legacy_image = bytearray(installed_image)
        layout = {
            "allocation_bitmap_start": struct.unpack_from("<Q", installed_image,
                                                              104)[0],
            "allocation_bitmap_blocks": struct.unpack_from("<Q", installed_image,
                                                               112)[0],
            "record_bitmap_start": struct.unpack_from("<Q", installed_image,
                                                        120)[0],
            "record_bitmap_blocks": struct.unpack_from("<Q", installed_image,
                                                         128)[0],
            "record_table_start": struct.unpack_from("<Q", installed_image,
                                                       136)[0],
            "record_table_blocks": struct.unpack_from("<Q", installed_image,
                                                        144)[0],
            "record_count": struct.unpack_from("<Q", installed_image,
                                                 152)[0],
            "data_start": struct.unpack_from("<Q", installed_image, 160)[0],
            "data_blocks": struct.unpack_from("<Q", installed_image, 168)[0],
        }

        def record(record_id):
            offset = update_mgfs.record_offset(
                installed_image, layout["record_table_start"], record_id,
                layout["record_count"])
            return installed_image[offset:offset + update_mgfs.RECORD_BYTES]

        def directory(record_id):
            return update_mgfs.record_payload(installed_image,
                                                record(record_id))

        share_directory_id = update_mgfs.directory_find(directory(1), "share")
        assert share_directory_id is not None
        info_directory_id = update_mgfs.directory_find(
            directory(share_directory_id), "info")
        assert info_directory_id is not None
        manual_entries = update_mgfs.directory_entries(directory(info_directory_id))
        table_indices = {update_mgfs.replace_directory_payload(
            legacy_image, layout, info_directory_id, b"", set())}
        for _offset, _size, manual_id, _flags, _name in manual_entries:
            table_indices.add(update_mgfs.release_record(
                legacy_image, layout, manual_id))
        for table_index in table_indices:
            update_mgfs.refresh_table_checksum(legacy_image, layout, table_index)
        update_mgfs.refresh_record_bitmap_checksum(legacy_image, layout)
        update_mgfs.refresh_metadata_checksums(legacy_image, layout)
        legacy_empty = pathlib.Path(temp) / "legacy-empty-info.img"
        legacy_empty.write_bytes(legacy_image)
        legacy_check = run(str(mgfsck), str(legacy_empty))
        assert legacy_check.returncode == 0, legacy_check.stderr
        update_mgfs.update(str(legacy_empty), payload_paths, data_paths)
        migrated_manuals, migrated_help = inspect_manuals(legacy_empty.read_bytes())
        assert {name: item[1] for name, item in migrated_manuals.items()} == \
            {name: item[1] for name, item in fresh_manuals.items()}
        assert {name: item[1] for name, item in migrated_help.items()} == \
            {name: item[1] for name, item in fresh_help.items()}
        assert run(str(mgfsck), str(legacy_empty)).returncode == 0

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
