#!/usr/bin/env python3
"""Update Mangrove's fixed system payloads without rebuilding user data."""

import math
import os
import struct
import sys

from populate_mgfs import (B, DEFAULT_NETWORK_CONFIG, MAGIC, checksum,
                           directory_entry, w64)

EXTENT_LIST_MAGIC = 0x315458455346474D
EXTENTS_PER_LIST_BLOCK = 126
RECORD_BYTES = 192
RECORDS_PER_TABLE_BLOCK = 21
METADATA_CHECKSUM_OFFSET = 16
RECORD_CHECKSUM_OFFSET = 184
SUPER_CHECKSUM_OFFSET = 192

SYSTEM_RECORDS = {
    8: "sprout",
    11: "shoot",
    10: "clear",
    13: "copy",
    14: "say",
    15: "uptime",
    12: "list",
    16: "locate",
    21: "move",
    22: "plant",
    23: "read",
    24: "remove",
    25: "version",
    26: "where",
    17: "ping",
    18: "resolve",
    19: "fetch",
    20: "network",
    27: "shutdown",
    28: "reboot",
    29: "power",
}

# This order is the public payload argument order used by make_image.sh.
# Keep it explicit: record IDs are not numerically ordered because the
# directory metadata occupies the intervening records.
PAYLOAD_RECORD_IDS = (8, 11, 10, 13, 14, 15, 12, 16, 21, 22, 23, 24,
                      25, 26, 17, 18, 19, 20, 27, 28, 29)

NETWORK_NAME = "network"
NETWORK_CONFIG_NAME = "config"


def u64(buf, offset):
    return struct.unpack_from("<Q", buf, offset)[0]


def record_offset(image, table_start, record_id, record_count):
    for slot in range(record_count):
        table_block = slot // RECORDS_PER_TABLE_BLOCK
        table_slot = slot % RECORDS_PER_TABLE_BLOCK
        offset = (table_start + table_block) * B + 24 + table_slot * RECORD_BYTES
        if u64(image, offset + 16) == record_id:
            return offset
    if 0 < record_id <= record_count:
        slot = record_id - 1
        table_block = slot // RECORDS_PER_TABLE_BLOCK
        table_slot = slot % RECORDS_PER_TABLE_BLOCK
        return (table_start + table_block) * B + 24 + table_slot * RECORD_BYTES
    raise RuntimeError("system Record ID %d is outside the record table" % record_id)


def record_slot_from_offset(offset, table_start):
    relative = offset - table_start * B
    if relative < 24 or (relative - 24) % RECORD_BYTES:
        raise RuntimeError("invalid MGFS Record table offset")
    table_block = relative // B
    slot_in_block = (relative % B - 24) // RECORD_BYTES
    if slot_in_block >= RECORDS_PER_TABLE_BLOCK:
        raise RuntimeError("invalid MGFS Record table slot")
    return table_block * RECORDS_PER_TABLE_BLOCK + slot_in_block


def record_table_offset(layout, slot):
    return (layout["record_table_start"] + slot // RECORDS_PER_TABLE_BLOCK) * B + \
        24 + (slot % RECORDS_PER_TABLE_BLOCK) * RECORD_BYTES


def record_slot_allocated(image, layout, slot):
    offset = layout["record_bitmap_start"] * B + 24 + slot // 8
    return bool(image[offset] & (1 << (slot % 8)))


def set_record_allocated(image, layout, slot, value):
    offset = layout["record_bitmap_start"] * B + 24 + slot // 8
    mask = 1 << (slot % 8)
    if value:
        image[offset] |= mask
    else:
        image[offset] &= ~mask & 0xff


def all_record_ids(image, layout):
    result = set()
    for slot in range(layout["record_count"]):
        offset = record_table_offset(layout, slot)
        record_id = u64(image, offset + 16)
        if record_id:
            result.add(record_id)
    return result


def allocate_record_slots(image, layout, count):
    slots = []
    for slot in range(layout["record_count"]):
        if record_slot_allocated(image, layout, slot):
            continue
        set_record_allocated(image, layout, slot, True)
        slots.append(slot)
        if len(slots) == count:
            return slots
    raise RuntimeError("MGFS Record table is full while adding network config")


def directory_entries(payload):
    entries = []
    offset = 0
    while offset < len(payload):
        if len(payload) - offset < 32:
            raise RuntimeError("truncated MGFS directory entry")
        name_length = u64(payload, offset + 8)
        entry_size = (32 + name_length + 7) & ~7
        if not name_length or entry_size > len(payload) - offset:
            raise RuntimeError("invalid MGFS directory entry")
        name = bytes(payload[offset + 32:offset + 32 + name_length])
        entries.append((offset, entry_size, u64(payload, offset),
                        u64(payload, offset + 16), name))
        offset += entry_size
    return entries


def directory_find(payload, name):
    wanted = name.encode("utf-8")
    for offset, size, record_id, flags, entry_name in directory_entries(payload):
        if flags == 1 and entry_name == wanted:
            return record_id
    return None


def extent_values(record, offset):
    return (u64(record, offset), u64(record, offset + 8),
            u64(record, offset + 16), u64(record, offset + 24))


def record_extents(image, record):
    extent_count = u64(record, 40)
    inline_count = u64(record, 48)
    list_block = u64(record, 56)
    extents = []
    list_blocks = []

    for index in range(inline_count):
        extents.append(extent_values(record, 64 + index * 32))

    while list_block:
        if list_block in list_blocks:
            raise RuntimeError("cyclic system extent-list chain")
        list_blocks.append(list_block)
        offset = list_block * B
        if u64(image, offset) != EXTENT_LIST_MAGIC:
            raise RuntimeError("invalid system extent-list block")
        entry_count = u64(image, offset + 24)
        if not entry_count or entry_count > EXTENTS_PER_LIST_BLOCK:
            raise RuntimeError("invalid system extent-list entry count")
        for index in range(entry_count):
            extents.append(extent_values(image, offset + 64 + index * 32))
        list_block = u64(image, offset + 16)

    if len(extents) != extent_count:
        raise RuntimeError("incomplete system extent-list chain")
    return extents, list_blocks


def record_payload(image, record):
    size = u64(record, 32)
    extents, _ = record_extents(image, record)
    payload = bytearray()
    for _, physical, count, flags in extents:
        if flags not in (1, 2):
            raise RuntimeError("invalid system extent")
        for block in range(physical, physical + count):
            offset = block * B
            payload.extend(image[offset:offset + B])
    return bytes(payload[:size])


def bitmap_location(layout, bit):
    bitmap_block = bit // 32576
    byte_offset = bitmap_block * B + 24 + (bit % 32576) // 8
    return layout["allocation_bitmap_start"] * B + byte_offset, 1 << (bit % 8)


def set_allocated(image, layout, physical_block, value):
    data_start = layout["data_start"]
    data_blocks = layout["data_blocks"]
    if physical_block < data_start or physical_block >= data_start + data_blocks:
        raise RuntimeError("system block lies outside the data area")
    offset, mask = bitmap_location(layout, physical_block - data_start)
    if value:
        image[offset] |= mask
    else:
        image[offset] &= ~mask & 0xff


def is_allocated(image, layout, physical_block):
    offset, mask = bitmap_location(layout, physical_block - layout["data_start"])
    return bool(image[offset] & mask)


def allocate_blocks(image, layout, count, reserved):
    blocks = []
    if count == 0:
        return blocks
    for physical in range(layout["data_start"],
                          layout["data_start"] + layout["data_blocks"]):
        if physical in reserved or is_allocated(image, layout, physical):
            continue
        set_allocated(image, layout, physical, True)
        reserved.add(physical)
        blocks.append(physical)
        if len(blocks) == count:
            return blocks
    raise RuntimeError("MGFS data area is full while updating system files")


def make_extents(blocks, flags=1):
    if not blocks:
        return []
    extents = []
    start = previous = blocks[0]
    for block in blocks[1:]:
        if block == previous + 1:
            previous = block
            continue
        extents.append((0, start, previous - start + 1, flags))
        start = previous = block
    extents.append((0, start, previous - start + 1, flags))
    return extents


def write_record(image, offset, record_id, record_type, generation, payload,
                 extents, list_head):
    record = bytearray(RECORD_BYTES)
    w64(record, 0, record_type)
    w64(record, 16, record_id)
    w64(record, 24, generation)
    w64(record, 32, len(payload))
    w64(record, 40, len(extents))
    w64(record, 48, min(2, len(extents)))
    w64(record, 56, list_head)
    for index, extent in enumerate(extents[:2]):
        for value_index, value in enumerate(extent):
            w64(record, 64 + index * 32 + value_index * 8, value)
    checksum(record, RECORD_CHECKSUM_OFFSET, RECORD_BYTES)
    image[offset:offset + RECORD_BYTES] = record


def write_extent_lists(image, record_id, extents, list_blocks):
    remaining = extents[2:]
    for index, physical in enumerate(list_blocks):
        block = bytearray(B)
        w64(block, 0, EXTENT_LIST_MAGIC)
        w64(block, 8, record_id)
        w64(block, 16, list_blocks[index + 1] if index + 1 < len(list_blocks) else 0)
        entries = remaining[index * EXTENTS_PER_LIST_BLOCK:
                           (index + 1) * EXTENTS_PER_LIST_BLOCK]
        w64(block, 24, len(entries))
        for entry_index, extent in enumerate(entries):
            for value_index, value in enumerate(extent):
                w64(block, 64 + entry_index * 32 + value_index * 8, value)
        checksum(block, 32, B)
        offset = physical * B
        image[offset:offset + B] = block


def replace_directory_payload(image, layout, record_id, payload, reserved):
    """Replace a directory payload while retaining its existing entries."""
    offset = record_offset(image, layout["record_table_start"], record_id,
                           layout["record_count"])
    old_record = bytes(image[offset:offset + RECORD_BYTES])
    old_extents, old_list_blocks = record_extents(image, old_record)
    old_data_blocks = []
    for _, physical, count, flags in old_extents:
        if flags != 2:
            raise RuntimeError("directory Record has non-directory extents")
        old_data_blocks.extend(range(physical, physical + count))

    needed = math.ceil(len(payload) / B)
    data_blocks = old_data_blocks[:]
    if len(data_blocks) < needed:
        data_blocks.extend(allocate_blocks(image, layout,
                                            needed - len(data_blocks), reserved))
    selected_blocks = data_blocks[:needed]
    extents = make_extents(selected_blocks, 2)
    list_count = math.ceil(max(0, len(extents) - 2) / EXTENTS_PER_LIST_BLOCK)
    list_blocks = allocate_blocks(image, layout, list_count, reserved)

    for index, block in enumerate(selected_blocks):
        start = index * B
        image[block * B:(block + 1) * B] = payload[start:start + B].ljust(B, b"\0")
    write_extent_lists(image, record_id, extents, list_blocks)
    write_record(image, offset, record_id, 2, u64(old_record, 24) + 1,
                 payload, extents, list_blocks[0] if list_blocks else 0)

    for block in old_data_blocks[len(selected_blocks):]:
        set_allocated(image, layout, block, False)
    for block in old_list_blocks:
        if block not in list_blocks:
            set_allocated(image, layout, block, False)
    return record_slot_from_offset(offset, layout["record_table_start"]) // \
        RECORDS_PER_TABLE_BLOCK


def refresh_table_checksum(image, layout, table_index):
    offset = (layout["record_table_start"] + table_index) * B
    block = bytearray(image[offset:offset + B])
    checksum(block, METADATA_CHECKSUM_OFFSET, B)
    image[offset:offset + B] = block


def refresh_record_bitmap_checksum(image, layout):
    for index in range(layout["record_bitmap_blocks"]):
        offset = (layout["record_bitmap_start"] + index) * B
        block = bytearray(image[offset:offset + B])
        checksum(block, METADATA_CHECKSUM_OFFSET, B)
        image[offset:offset + B] = block


def ensure_network_config(image, layout):
    """Add the default persistent config only to legacy images missing it."""
    root_offset = record_offset(image, layout["record_table_start"], 1,
                                layout["record_count"])
    root_payload = record_payload(image, bytes(image[root_offset:
                                                    root_offset + RECORD_BYTES]))
    core_id = directory_find(root_payload, "core")
    if core_id is None:
        raise RuntimeError("root directory has no core directory")
    core_offset = record_offset(image, layout["record_table_start"], core_id,
                                layout["record_count"])
    core_record = bytes(image[core_offset:core_offset + RECORD_BYTES])
    core_payload = record_payload(image, core_record)
    network_id = directory_find(core_payload, NETWORK_NAME)
    if network_id is not None:
        network_offset = record_offset(image, layout["record_table_start"],
                                       network_id, layout["record_count"])
        network_record = bytes(image[network_offset:network_offset + RECORD_BYTES])
        if u64(network_record, 0) != 2:
            raise RuntimeError("core/network is not a directory")
        network_payload = record_payload(image, network_record)
    else:
        network_payload = b""

    config_id = directory_find(network_payload, NETWORK_CONFIG_NAME)
    if config_id is not None:
        config_offset = record_offset(image, layout["record_table_start"],
                                      config_id, layout["record_count"])
        config_record = bytes(image[config_offset:config_offset + RECORD_BYTES])
        if u64(config_record, 0) != 1:
            raise RuntimeError("core/network/config is not a file")
        return False

    used_ids = all_record_ids(image, layout)
    next_id = max(2, u64(image, 96), max(used_ids, default=1) + 1)
    slots = allocate_record_slots(image, layout, 1 if network_id is not None else 2)
    slot_index = 0

    if network_id is None:
        while next_id in used_ids:
            next_id += 1
        network_id = next_id
        next_id += 1
        used_ids.add(network_id)
        network_slot = slots[slot_index]
        slot_index += 1
    else:
        network_slot = None

    while next_id in used_ids:
        next_id += 1
    config_id = next_id
    next_id += 1
    config_slot = slots[slot_index]

    reserved = set()
    config_blocks = allocate_blocks(image, layout,
                                     math.ceil(len(DEFAULT_NETWORK_CONFIG) / B),
                                     reserved)
    for index, block in enumerate(config_blocks):
        start = index * B
        image[block * B:(block + 1) * B] = DEFAULT_NETWORK_CONFIG[
            start:start + B].ljust(B, b"\0")
    config_offset = record_table_offset(layout, config_slot)
    write_record(image, config_offset, config_id, 1, 1,
                 DEFAULT_NETWORK_CONFIG, make_extents(config_blocks, 1), 0)

    network_payload += directory_entry(config_id, NETWORK_CONFIG_NAME)
    table_indices = {config_offset // B - layout["record_table_start"]}
    if network_id is None:
        raise RuntimeError("failed to allocate network directory")
    if network_slot is not None:
        network_offset = record_table_offset(layout, network_slot)
        network_blocks = allocate_blocks(image, layout,
                                         math.ceil(len(network_payload) / B),
                                         reserved)
        for index, block in enumerate(network_blocks):
            start = index * B
            image[block * B:(block + 1) * B] = network_payload[
                start:start + B].ljust(B, b"\0")
        write_record(image, network_offset, network_id, 2, 1,
                     network_payload, make_extents(network_blocks, 2), 0)
        table_indices.add(network_offset // B - layout["record_table_start"])
        core_payload += directory_entry(network_id, NETWORK_NAME)
    else:
        table_indices.add(replace_directory_payload(
            image, layout, network_id, network_payload, reserved))

    table_indices.add(replace_directory_payload(
        image, layout, core_id, core_payload, reserved))
    for table_index in table_indices:
        refresh_table_checksum(image, layout, table_index)
    refresh_record_bitmap_checksum(image, layout)
    refresh_metadata_checksums(image, layout)
    superblock = bytearray(image[:B])
    w64(superblock, 96, next_id)
    checksum(superblock, SUPER_CHECKSUM_OFFSET, 200)
    image[:B] = superblock
    return True


def refresh_metadata_checksums(image, layout):
    for index in range(layout["allocation_bitmap_blocks"]):
        offset = (layout["allocation_bitmap_start"] + index) * B
        block = bytearray(image[offset:offset + B])
        checksum(block, METADATA_CHECKSUM_OFFSET, B)
        image[offset:offset + B] = block


def update(image_path, payload_paths):
    image = bytearray(open(image_path, "rb").read())
    if image[:8] != MAGIC:
        raise RuntimeError("not an MGFS v1 image")

    total_blocks = u64(image, 40)
    if total_blocks * B != len(image):
        raise RuntimeError("unsupported MGFS geometry")
    layout = {
        "allocation_bitmap_start": u64(image, 104),
        "allocation_bitmap_blocks": u64(image, 112),
        "record_bitmap_start": u64(image, 120),
        "record_bitmap_blocks": u64(image, 128),
        "record_table_start": u64(image, 136),
        "record_table_blocks": u64(image, 144),
        "record_count": u64(image, 152),
        "data_start": u64(image, 160),
        "data_blocks": u64(image, 168),
    }
    if len(payload_paths) != len(PAYLOAD_RECORD_IDS):
        raise RuntimeError("wrong number of system payloads")
    payloads = {record_id: open(path, "rb").read()
                for record_id, path in zip(PAYLOAD_RECORD_IDS, payload_paths)}
    bin_data = b"".join(directory_entry(record_id, name)
                         for record_id, name in SYSTEM_RECORDS.items())
    active_records = [2] + list(SYSTEM_RECORDS)
    persistent_changed = ensure_network_config(image, layout)
    offsets = {record_id: record_offset(image, layout["record_table_start"],
                                         record_id, layout["record_count"])
               for record_id in active_records}

    if (record_payload(image, bytes(image[offsets[2]:offsets[2] + RECORD_BYTES])) ==
            bin_data and
            all(record_payload(image, bytes(image[offsets[record_id]:
                                                  offsets[record_id] + RECORD_BYTES])) ==
                payloads[record_id] for record_id in SYSTEM_RECORDS) and
            not persistent_changed):
        return False

    reserved = set()
    old_state = {}
    for record_id, offset in offsets.items():
        record = bytes(image[offset:offset + RECORD_BYTES])
        extents, list_blocks = record_extents(image, record)
        data_blocks = []
        expected_logical = 0
        for logical, physical, count, flags in extents:
            if logical != expected_logical:
                raise RuntimeError("non-sequential system extents")
            if flags not in (1, 2):
                raise RuntimeError("invalid system extent")
            data_blocks.extend(range(physical, physical + count))
            expected_logical += count
        old_state[record_id] = (u64(record, 24), extents, list_blocks, data_blocks)
        for block in data_blocks + list_blocks:
            set_allocated(image, layout, block, False)

    for record_id, offset in offsets.items():
        payload = bin_data if record_id == 2 else payloads[record_id]
        needed = math.ceil(len(payload) / B)
        data_blocks = allocate_blocks(image, layout, needed, reserved)
        extents = make_extents(data_blocks, 2 if record_id == 2 else 1)
        list_count = math.ceil(max(0, len(extents) - 2) / EXTENTS_PER_LIST_BLOCK)
        list_blocks = allocate_blocks(image, layout, list_count, reserved)
        for index, block in enumerate(data_blocks):
            start = index * B
            image[block * B:(block + 1) * B] = payload[start:start + B].ljust(B, b"\0")
        write_extent_lists(image, record_id, extents, list_blocks)
        generation = old_state[record_id][0] + 1
        write_record(image, offset, record_id, 2 if record_id == 2 else 1,
                     generation, payload, extents,
                     list_blocks[0] if list_blocks else 0)

    table_indices = {
        (offset - layout["record_table_start"] * B) // B
        for offset in offsets.values()
    }
    for index in table_indices:
        offset = (layout["record_table_start"] + index) * B
        block = bytearray(image[offset:offset + B])
        checksum(block, METADATA_CHECKSUM_OFFSET, B)
        image[offset:offset + B] = block

    record_bitmap = bytearray(image[layout["record_bitmap_start"] * B:
                                    (layout["record_bitmap_start"] + 1) * B])
    for record_id in active_records:
        bit = record_id - 1
        record_bitmap[24 + bit // 8] |= 1 << (bit % 8)
    checksum(record_bitmap, METADATA_CHECKSUM_OFFSET, B)
    image[layout["record_bitmap_start"] * B:
          (layout["record_bitmap_start"] + 1) * B] = record_bitmap

    refresh_metadata_checksums(image, layout)
    superblock = bytearray(image[:B])
    w64(superblock, 64, 1)
    checksum(superblock, SUPER_CHECKSUM_OFFSET, 200)
    image[:B] = superblock

    temporary = image_path + ".update-tmp"
    with open(temporary, "wb") as output:
        output.write(image)
    os.replace(temporary, image_path)
    return True


def main():
    if len(sys.argv) != 23:
        raise SystemExit("usage: update_mgfs.py <image> "
                         "<sprout-elf> <shoot-elf> <clear-elf> <copy-elf> "
                         "<say-elf> <uptime-elf> <list-elf> <locate-elf> "
                         "<move-elf> <plant-elf> <read-elf> <remove-elf> "
                         "<version-elf> <where-elf> <ping-elf> <resolve-elf> "
                         "<fetch-elf> <network-elf> <shutdown-elf> "
                         "<reboot-elf> <power-elf>")
    try:
        update(sys.argv[1], sys.argv[2:])
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit("update_mgfs: %s" % error)


if __name__ == "__main__":
    main()
