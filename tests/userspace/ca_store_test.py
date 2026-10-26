#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate CA provenance and fresh/incremental MGFS trust-store packaging."""
from copy import deepcopy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path[:0] = [str(ROOT / "tools"),
                str(ROOT / "tests" / "filesystems" / "mgfs")]
import ca_store
import populate_mgfs as populate
import update_mgfs as update


def rejected(call):
    try:
        call()
    except (OSError, ValueError):
        return
    raise AssertionError("invalid CA store was accepted")


def check_image(mgfsck, image):
    result = subprocess.run([str(mgfsck), str(image)], text=True,
                            capture_output=True, check=True)
    if "Errors: 0\n" not in result.stdout or "Warnings: 0\n" not in result.stdout:
        raise AssertionError("independent mgfsck did not report clean media")


def record_payload(image, identifier):
    layout = update.image_layout(image) if hasattr(update, "image_layout") else {
        name: update.u64(image, offset) for name, offset in (
            ("record_table_start", 136), ("record_count", 152))
    }
    offset = update.record_offset(image, layout["record_table_start"],
                                  identifier, layout["record_count"])
    return update.record_payload(image,
                                 bytes(image[offset:offset + update.RECORD_BYTES]))


def installed_ca_files(image):
    root_payload = record_payload(image, 1)
    share_id = update.directory_find(root_payload, "share")
    share_payload = record_payload(image, share_id)
    store_id = update.directory_find(share_payload, "ca-certificates")
    if store_id is None:
        raise AssertionError("/share/ca-certificates is absent")
    store_payload = record_payload(image, store_id)
    files = {}
    for name in ca_store.load_payloads():
        record_id = update.directory_find(store_payload, name)
        if record_id is None:
            raise AssertionError("CA store file is absent: " + name)
        offset = update.record_offset(image, update.u64(image, 136), record_id,
                                      update.u64(image, 152))
        record = bytes(image[offset:offset + update.RECORD_BYTES])
        files[name] = (update.record_payload(image, record), record)
    return store_id, files


def main():
    mkmgfs = Path(sys.argv[1]).resolve()
    mgfsck = Path(sys.argv[2]).resolve()
    manifest = ca_store.verify_store()
    bundle = (ca_store.STORE / ca_store.BUNDLE_NAME).read_bytes()
    source_bundle = ca_store.STORE / ca_store.BUNDLE_NAME
    license_bytes = (ca_store.STORE / ca_store.LICENSE_NAME).read_bytes()
    assert manifest["certificate_count"] == 118
    assert manifest["license"] == "MPL-2.0"

    with tempfile.TemporaryDirectory(prefix="mangrove-ca-negative-") as temp:
        directory = Path(temp)
        license_path = directory / ca_store.LICENSE_NAME
        license_path.write_bytes(license_bytes)
        good_manifest = directory / ca_store.MANIFEST_NAME
        good_manifest.write_text(json.dumps(manifest), encoding="utf-8")
        rejected(lambda: ca_store.verify_store(
            directory / "missing.pem", good_manifest, license_path))
        empty = directory / "empty.pem"
        empty.write_bytes(b"")
        rejected(lambda: ca_store.verify_store(empty, good_manifest, license_path))
        malformed = directory / "malformed.pem"
        malformed.write_bytes(b"-----BEGIN CERTIFICATE-----\n@@\n")
        rejected(lambda: ca_store.verify_store(malformed, good_manifest,
                                               license_path))
        mismatch = directory / "mismatch.pem"
        mismatch.write_bytes(bundle[:-1] + bytes([bundle[-1] ^ 1]))
        rejected(lambda: ca_store.verify_store(mismatch, good_manifest,
                                               license_path))
        malformed_manifest = directory / "bad-manifest.json"
        malformed_manifest.write_text("{", encoding="utf-8")
        rejected(lambda: ca_store.verify_store(
            ca_store.STORE / ca_store.BUNDLE_NAME, malformed_manifest,
            license_path))
        rejected(lambda: ca_store.verify_store(
            ca_store.STORE / ca_store.BUNDLE_NAME, good_manifest,
            directory / "absent-license"))
        altered = deepcopy(manifest)
        altered["certificate_count"] += 1
        good_manifest.write_text(json.dumps(altered), encoding="utf-8")
        rejected(lambda: ca_store.verify_store(
            ca_store.STORE / ca_store.BUNDLE_NAME, good_manifest,
            license_path))
        before = (ca_store.STORE / ca_store.BUNDLE_NAME).read_bytes()
        rejected(lambda: ca_store.import_bundle(malformed))
        assert (ca_store.STORE / ca_store.BUNDLE_NAME).read_bytes() == before

    with tempfile.TemporaryDirectory(prefix="mangrove-ca-import-") as temp:
        original_store = ca_store.STORE
        imported_store = Path(temp) / "share" / "ca-certificates"
        try:
            ca_store.STORE = imported_store
            (imported_store / ca_store.LICENSE_NAME).parent.mkdir(parents=True)
            (imported_store / ca_store.LICENSE_NAME).write_bytes(license_bytes)
            assert ca_store.import_bundle(source_bundle) == manifest
            assert ca_store.verify_store() == manifest
            assert (imported_store / ca_store.BUNDLE_NAME).read_bytes() == bundle
            assert (imported_store / ca_store.BUNDLE_NAME).stat().st_mode & 0o777 == 0o644
            assert (imported_store / ca_store.MANIFEST_NAME).stat().st_mode & 0o777 == 0o644
        finally:
            ca_store.STORE = original_store

    with tempfile.TemporaryDirectory(prefix="mangrove-ca-image-") as temp:
        directory = Path(temp)
        image_path = directory / "root.img"
        dummy = directory / "payload.bin"
        dummy.write_bytes(b"image-payload-fixture\n")
        subprocess.run([str(mkmgfs), "--blocks", "16384", "--uuid",
                        "12345678-1234-5678-9abc-def012345678",
                        "--format-time-ns", "1", str(image_path)],
                       check=True, capture_output=True)
        binary_paths = [str(dummy)] * len(populate.PAYLOAD_MANIFEST)
        data_paths = [str(dummy)] * len(populate.DATA_MANIFEST)
        subprocess.run([sys.executable, str(ROOT / "tools/populate_mgfs.py"),
                        str(image_path), *binary_paths, *data_paths],
                       check=True, capture_output=True)
        check_image(mgfsck, image_path)
        image = bytearray(image_path.read_bytes())
        store_id, installed = installed_ca_files(image)
        store_offset = update.record_offset(
            image, update.u64(image, 136), store_id, update.u64(image, 152))
        store_record = bytes(image[store_offset:store_offset + update.RECORD_BYTES])
        assert update.record_owner(store_record) == 0
        assert update.record_permissions(store_record) == populate.SYSTEM_PERMISSIONS
        for name, expected in ca_store.load_payloads().items():
            content, record = installed[name]
            assert content == expected
            assert update.record_owner(record) == 0
            permissions = update.record_permissions(record)
            assert permissions & populate.PERMISSION_OTHER_READ
            assert not permissions & populate.PERMISSION_OTHER_WRITE

        # Model an older managed bundle with a valid MGFS mutation. The
        # updater must replace it from the reviewed source, then become
        # idempotent.
        root_payload = record_payload(image, 1)
        share_id = update.directory_find(root_payload, "share")
        share_payload = record_payload(image, share_id)
        directory_id = update.directory_find(share_payload, "ca-certificates")
        directory_payload = record_payload(image, directory_id)
        bundle_id = update.directory_find(directory_payload, ca_store.BUNDLE_NAME)
        layout = {name: update.u64(image, offset) for name, offset in (
            ("allocation_bitmap_start", 104), ("allocation_bitmap_blocks", 112),
            ("record_bitmap_start", 120), ("record_bitmap_blocks", 128),
            ("record_table_start", 136), ("record_table_blocks", 144),
            ("record_count", 152), ("data_start", 160), ("data_blocks", 168))}
        table_index = update.replace_file_payload(
            image, layout, bundle_id, b"older CA bundle fixture\n", set())
        update.refresh_table_checksum(image, layout, table_index)
        update.refresh_metadata_checksums(image, layout)
        image_path.write_bytes(image)
        check_image(mgfsck, image_path)
        assert update.update(str(image_path), binary_paths, data_paths) is True
        assert update.update(str(image_path), binary_paths, data_paths) is False
        check_image(mgfsck, image_path)
        updated = bytearray(image_path.read_bytes())
        _store_id, updated_files = installed_ca_files(updated)
        for name, expected in ca_store.load_payloads().items():
            assert updated_files[name][0] == expected

        # An unexpected user-owned managed path is not silently adopted or
        # overwritten by an administrator image update.
        root_payload = record_payload(updated, 1)
        share_id = update.directory_find(root_payload, "share")
        share_payload = record_payload(updated, share_id)
        directory_id = update.directory_find(share_payload, "ca-certificates")
        directory_payload = record_payload(updated, directory_id)
        bundle_id = update.directory_find(directory_payload, ca_store.BUNDLE_NAME)
        bundle_offset = update.record_offset(
            updated, update.u64(updated, 136), bundle_id, update.u64(updated, 152))
        bundle_record = bytearray(updated[
            bundle_offset:bundle_offset + update.RECORD_BYTES])
        flags = update.u64(bundle_record, 8)
        flags &= ~(((1 << 32) - 1) << update.RECORD_OWNER_SHIFT)
        flags |= 1000 << update.RECORD_OWNER_SHIFT
        update.w64(bundle_record, 8, flags)
        update.checksum(bundle_record, update.RECORD_CHECKSUM_OFFSET,
                        update.RECORD_BYTES)
        updated[bundle_offset:bundle_offset + update.RECORD_BYTES] = bundle_record
        table_index = update.record_slot_from_offset(
            bundle_offset, update.u64(updated, 136)) // update.RECORDS_PER_TABLE_BLOCK
        update.refresh_table_checksum(updated, layout, table_index)
        owner_conflict = directory / "owner-conflict.img"
        owner_conflict.write_bytes(updated)
        check_image(mgfsck, owner_conflict)
        before = owner_conflict.read_bytes()
        try:
            update.update(str(owner_conflict), binary_paths, data_paths)
        except RuntimeError as error:
            assert "not system-owned" in str(error)
        else:
            raise AssertionError("updater adopted a user-owned CA store file")
        assert owner_conflict.read_bytes() == before

    print("CA store: 118 parsed Mozilla roots, fail-closed validation, "
          "system ownership/read-only permissions, fresh and incremental "
          "MGFS installation passed")


if __name__ == "__main__":
    main()
