#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise updater reconciliation of stale MGFS Record-table bytes."""

import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import populate_mgfs  # noqa: E402
import update_mgfs  # noqa: E402


def run(*args):
    return subprocess.run(args, capture_output=True, text=True)


def image_layout(image):
    return {
        "allocation_bitmap_start": update_mgfs.u64(image, 104),
        "allocation_bitmap_blocks": update_mgfs.u64(image, 112),
        "record_bitmap_start": update_mgfs.u64(image, 120),
        "record_bitmap_blocks": update_mgfs.u64(image, 128),
        "record_table_start": update_mgfs.u64(image, 136),
        "record_table_blocks": update_mgfs.u64(image, 144),
        "record_count": update_mgfs.u64(image, 152),
        "data_start": update_mgfs.u64(image, 160),
        "data_blocks": update_mgfs.u64(image, 168),
    }


def live_file_with_one_extent(image, layout):
    for slot in range(layout["record_count"]):
        if not update_mgfs.record_slot_allocated(image, layout, slot):
            continue
        offset = update_mgfs.record_table_offset(layout, slot)
        record = bytes(image[offset:offset + update_mgfs.RECORD_BYTES])
        if (update_mgfs.u64(record, 0) == 1 and
                not update_mgfs.u64(record, 8) & populate_mgfs.RECORD_INLINE_DATA and
                update_mgfs.u64(record, 40) == 1 and
                update_mgfs.u64(record, 48) == 1 and
                update_mgfs.u64(record, 56) == 0):
            return slot, offset, record
    raise AssertionError("fixture has no active one-extent file")


def first_free_slot(image, layout):
    for slot in range(layout["record_count"]):
        if not update_mgfs.record_slot_allocated(image, layout, slot):
            return slot
    raise AssertionError("fixture has no free Record slot")


def unused_record_id(image, layout):
    active = set()
    for slot in range(layout["record_count"]):
        if update_mgfs.record_slot_allocated(image, layout, slot):
            offset = update_mgfs.record_table_offset(layout, slot)
            active.add(update_mgfs.u64(image, offset + 16))
    for record_id in range(layout["record_count"], 0, -1):
        if record_id not in active:
            return record_id
    raise AssertionError("fixture has no unused Record ID")


def add_free_tombstone(image, layout, active_record, data_block):
    slot = first_free_slot(image, layout)
    offset = update_mgfs.record_table_offset(layout, slot)
    stale = bytearray(active_record)
    update_mgfs.w64(stale, 16, unused_record_id(image, layout))
    update_mgfs.w64(stale, 24, 1)
    update_mgfs.w64(stale, 72, data_block)
    update_mgfs.checksum(stale, update_mgfs.RECORD_CHECKSUM_OFFSET,
                         update_mgfs.RECORD_BYTES)
    image[offset:offset + update_mgfs.RECORD_BYTES] = stale
    update_mgfs.refresh_table_checksum(image, layout,
                                       slot // update_mgfs.RECORDS_PER_TABLE_BLOCK)
    return slot, offset


def checker_ok(mgfsck, path):
    result = run(str(mgfsck), str(path))
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Errors: 0" in result.stdout, result.stdout
    assert "Warnings: 0" in result.stdout, result.stdout


def large_id_updates(source, layout, directory, payloads, data_payloads, mgfsck):
    """Real updater publications and independent checking, on copied media."""
    def record(image, identifier):
        offset = update_mgfs.record_offset(
            image, layout["record_table_start"], identifier, layout["record_count"])
        return bytes(image[offset:offset + update_mgfs.RECORD_BYTES])

    def directory_payload(image, identifier):
        return update_mgfs.record_payload(image, record(image, identifier))

    def finish(image):
        update_mgfs.refresh_record_bitmap_checksum(image, layout)
        update_mgfs.refresh_metadata_checksums(image, layout)
        populate_mgfs.checksum(image, 192, 200)

    # A valid runtime-style object can have an ID far beyond the slot count.
    high_id = (1 << 32) + 17
    image = bytearray(source)
    slot = update_mgfs.allocate_record_slots(image, layout, 1)[0]
    contents = b"PRESERVE USER DATA\n" * 512
    table_index = update_mgfs._create_payload_record(
        image, layout, high_id, slot, 1, contents, 1, set(),
        owner_uid=1000, permissions=populate_mgfs.USER_PERMISSIONS)
    update_mgfs.refresh_table_checksum(image, layout, table_index)
    table_index = update_mgfs.replace_directory_payload(
        image, layout, 1, directory_payload(image, 1) +
        populate_mgfs.directory_entry(high_id, "large-id-user"), set())
    update_mgfs.refresh_table_checksum(image, layout, table_index)
    update_mgfs.w64(image, 96, high_id + 1)
    finish(image)
    path = directory / "large-id-user.img"
    path.write_bytes(image)
    checker_ok(mgfsck, path)
    before = bytes(image)
    owners = update_mgfs._live_record_block_owners(image, layout)
    assert any(owner[1] == high_id for owner in owners.values())
    assert not update_mgfs.reconcile_record_bitmap(image, layout)
    assert bytes(image) == before
    invalid = bytearray(image)
    at = update_mgfs.record_table_offset(layout, slot)
    update_mgfs.w64(invalid, at + 16, update_mgfs.u64(invalid, 96))
    damaged_record = bytearray(invalid[at:at + update_mgfs.RECORD_BYTES])
    populate_mgfs.checksum(damaged_record, 184, 192)
    invalid[at:at + update_mgfs.RECORD_BYTES] = damaged_record
    update_mgfs.refresh_table_checksum(invalid, layout, slot // 21)
    invalid_before = bytes(invalid)
    try:
        update_mgfs.reconcile_record_bitmap(invalid, layout)
    except RuntimeError as error:
        assert "malformed" in str(error)
    else:
        raise AssertionError("accepted an unconsumed Record identity")
    assert bytes(invalid) == invalid_before
    update_mgfs.update(str(path), payloads, data_payloads)
    updated = bytearray(path.read_bytes())
    assert directory_payload(updated, 1) == directory_payload(image, 1)
    assert record(updated, high_id) == record(image, high_id)
    assert update_mgfs.record_payload(updated, record(updated, high_id)) == contents
    checker_ok(mgfsck, path)

    # An older image missing one manual has a free slot but no unused small
    # identity. Installing its replacement must allocate a new monotonic ID.
    share_id = update_mgfs.directory_find(directory_payload(image, 1), "share")
    info_id = update_mgfs.directory_find(directory_payload(image, share_id), "info")
    old_payload = directory_payload(image, info_id)
    old_id = update_mgfs.directory_find(old_payload, "shoot")
    table_index = update_mgfs.replace_directory_payload(
        image, layout, info_id, update_mgfs.directory_remove(old_payload, "shoot"), set())
    update_mgfs.refresh_table_checksum(image, layout, table_index)
    table_index = update_mgfs.release_record(image, layout, old_id)
    update_mgfs.refresh_table_checksum(image, layout, table_index)
    finish(image)
    path = directory / "large-id-manual.img"
    path.write_bytes(image)
    checker_ok(mgfsck, path)
    exhausted = bytearray(image)
    update_mgfs.w64(exhausted, 96, (1 << 64) - 1)
    finish(exhausted)
    exhausted_path = directory / "exhausted-identity.img"
    exhausted_path.write_bytes(exhausted)
    checker_ok(mgfsck, exhausted_path)
    try:
        update_mgfs.update(str(exhausted_path), payloads, data_payloads)
    except RuntimeError as error:
        assert "Record ID space is exhausted" in str(error)
    else:
        raise AssertionError("allocated beyond the 64-bit Record identity space")
    assert exhausted_path.read_bytes() == exhausted
    update_mgfs.update(str(path), payloads, data_payloads)
    updated = bytearray(path.read_bytes())
    new_id = update_mgfs.directory_find(directory_payload(updated, info_id), "shoot")
    assert new_id >= high_id + 1 and new_id != old_id
    assert update_mgfs.record_payload(updated, record(updated, new_id)) == \
        (ROOT / "share/info/shoot").read_bytes()
    assert record(updated, high_id) == record(image, high_id)
    assert update_mgfs.record_payload(updated, record(updated, high_id)) == contents
    checker_ok(mgfsck, path)
    before = path.read_bytes()
    update_mgfs.update(str(path), payloads, data_payloads)
    assert path.read_bytes() == before, "repeated update changed an up-to-date image"

    # Preserve an old manual subtree while creating a replacement /share/info.
    # Its canonical ID is already consumed, so the directory itself also needs
    # a large ID in a different free table slot.
    image = bytearray(before)
    payload = update_mgfs.directory_remove(directory_payload(image, share_id), "info")
    payload += populate_mgfs.directory_entry(info_id, "archive")
    table_index = update_mgfs.replace_directory_payload(
        image, layout, share_id, payload, set())
    update_mgfs.refresh_table_checksum(image, layout, table_index)
    finish(image)
    path = directory / "large-id-manual-directory.img"
    path.write_bytes(image)
    checker_ok(mgfsck, path)
    update_mgfs.update(str(path), payloads, data_payloads)
    updated = bytearray(path.read_bytes())
    new_info = update_mgfs.directory_find(directory_payload(updated, share_id), "info")
    assert new_info > layout["record_count"] and new_info != info_id
    assert update_mgfs.directory_find(directory_payload(updated, share_id), "archive") == info_id
    assert record(updated, high_id) == record(image, high_id)
    checker_ok(mgfsck, path)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: mgfs_update_record_reconcile_test.py <mkmgfs> <mgfsck>")
    mkmgfs, mgfsck = map(pathlib.Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="mangrove-mgfs-record-reconcile-") as temp:
        directory = pathlib.Path(temp)
        base = directory / "base.img"
        payload = directory / "payload"
        payload.write_bytes(b"fixture-payload" * 64)
        subprocess.run([str(mkmgfs), "--blocks", "3600", "--uuid",
                        "11111111-2222-3333-4444-555555555555",
                        "--format-time-ns", "1", str(base)], check=True)
        payloads = [str(payload)] * len(populate_mgfs.PAYLOAD_MANIFEST)
        data_payloads = [str(payload)] * len(populate_mgfs.DATA_MANIFEST)
        populated = run(sys.executable, str(ROOT / "tools/populate_mgfs.py"),
                        str(base), *payloads, *data_payloads)
        assert populated.returncode == 0, populated.stdout + populated.stderr
        checker_ok(mgfsck, base)

        source = bytearray(base.read_bytes())
        layout = image_layout(source)
        _active_slot, _active_offset, active_record = live_file_with_one_extent(
            source, layout)
        active_data_block = update_mgfs.u64(active_record, 72)
        active_record_id = update_mgfs.u64(active_record, 16)
        active_block_bytes = bytes(source[active_data_block * populate_mgfs.B:
                                          (active_data_block + 1) * populate_mgfs.B])

        # A deleted slot may still contain its old extent after those blocks
        # have been reused by exactly one current bitmap-allocated Record.
        reused = bytearray(source)
        stale_slot, stale_offset = add_free_tombstone(
            reused, layout, active_record, active_data_block)
        reused_path = directory / "reused.img"
        reused_path.write_bytes(reused)
        checker_ok(mgfsck, reused_path)
        owners = update_mgfs._live_record_block_owners(reused, layout)
        assert owners[active_data_block][1] == active_record_id
        assert update_mgfs.reconcile_record_bitmap(reused, layout)
        assert reused[stale_offset:stale_offset + update_mgfs.RECORD_BYTES] == \
            b"\0" * update_mgfs.RECORD_BYTES
        assert not update_mgfs.record_slot_allocated(reused, layout, stale_slot)
        assert update_mgfs.record_slot_allocated(reused, layout, _active_slot)
        assert bytes(reused[active_data_block * populate_mgfs.B:
                            (active_data_block + 1) * populate_mgfs.B]) == \
            active_block_bytes
        assert update_mgfs.is_allocated(reused, layout, active_data_block)
        reconciled_path = directory / "reconciled.img"
        reconciled_path.write_bytes(reused)
        checker_ok(mgfsck, reconciled_path)

        # A genuinely released extent is also safe to remove from a stale
        # record; its bitmap bit is already clear.
        released = bytearray(source)
        free_block = next(
            block for block in range(layout["data_start"],
                                     layout["data_start"] + layout["data_blocks"])
            if not update_mgfs.is_allocated(released, layout, block))
        free_slot, free_offset = add_free_tombstone(
            released, layout, active_record, free_block)
        assert update_mgfs.reconcile_record_bitmap(released, layout)
        assert released[free_offset:free_offset + update_mgfs.RECORD_BYTES] == \
            b"\0" * update_mgfs.RECORD_BYTES
        assert not update_mgfs.record_slot_allocated(released, layout, free_slot)
        released_path = directory / "released.img"
        released_path.write_bytes(released)
        checker_ok(mgfsck, released_path)

        # An allocated reference with no live Record owner is not tombstone
        # reuse. Keep the existing hard error and leave every byte untouched.
        unsafe = bytearray(source)
        orphan_block = next(
            block for block in range(layout["data_start"],
                                     layout["data_start"] + layout["data_blocks"])
            if not update_mgfs.is_allocated(unsafe, layout, block))
        unsafe_slot, _unsafe_offset = add_free_tombstone(
            unsafe, layout, active_record, orphan_block)
        update_mgfs.set_allocated(unsafe, layout, orphan_block, True)
        update_mgfs.refresh_metadata_checksums(unsafe, layout)
        unsafe_path = directory / "unsafe-orphan.img"
        unsafe_path.write_bytes(unsafe)
        unsafe_check = run(str(mgfsck), str(unsafe_path))
        assert unsafe_check.returncode == 0, unsafe_check.stderr
        assert "Unowned allocated blocks: 1" in unsafe_check.stdout, \
            unsafe_check.stdout
        assert "Warnings: 1" in unsafe_check.stdout, unsafe_check.stdout
        unsafe_before = bytes(unsafe)
        try:
            update_mgfs.reconcile_record_bitmap(unsafe, layout)
        except RuntimeError as error:
            assert "without a live owner" in str(error), str(error)
            assert "slot %d" % unsafe_slot in str(error), str(error)
        else:
            raise AssertionError("reconciler accepted an unowned allocated extent")
        assert bytes(unsafe) == unsafe_before, \
            "failed reconciliation modified the image in memory"

        large_id_updates(source, layout, directory, payloads, data_payloads, mgfsck)

    print("PASS: stale Record reuse, released extents, unowned-allocation rejection, "
          "large live IDs and monotonic manual installation")


if __name__ == "__main__":
    main()
