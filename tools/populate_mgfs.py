#!/usr/bin/env python3
import math
import struct
import sys

B = 4096
CRC_POLY = 0x42F0E1EBA9EA3693
MAGIC = b"MGFSv1\0\0"

def w64(buf, off, value):
    struct.pack_into("<Q", buf, off, value)

def crc64(data):
    crc = 0
    for byte in data:
        crc ^= byte << 56
        for _ in range(8):
            crc = ((crc << 1) ^ CRC_POLY) & ((1 << 64) - 1) if crc & (1 << 63) else (crc << 1) & ((1 << 64) - 1)
    return crc

def checksum(buf, offset, length):
    w64(buf, offset, 0)
    w64(buf, offset, crc64(buf[:length]))

def extent(record, offset, logical, physical, count, flags=1):
    for value in (logical, physical, count, flags):
        w64(record, offset, value)
        offset += 8

def make_record(record_id, record_type, size=0, extents=()):
    record = bytearray(192)
    w64(record, 0, record_type)
    w64(record, 16, record_id)
    w64(record, 24, 1)
    w64(record, 32, size)
    w64(record, 40, len(extents))
    w64(record, 48, min(2, len(extents)))
    for index, item in enumerate(extents[:2]):
        extent(record, 64 + index * 32, *item)
    if len(extents) > 2:
        w64(record, 56, extents[2][1])
    checksum(record, 184, 192)
    return record

def record_payload_matches(image, table_start, record_id, payload):
    """Return whether a regular-file Record contains exactly payload."""
    slot = record_id - 1
    record_offset = (table_start + slot // 21) * B + 24 + (slot % 21) * 192
    if record_offset + 192 > len(image):
        return False
    record = image[record_offset:record_offset + 192]
    if struct.unpack_from("<Q", record, 0)[0] != 1:
        return False
    if struct.unpack_from("<Q", record, 32)[0] != len(payload):
        return False
    extent_count = struct.unpack_from("<Q", record, 40)[0]
    direct_count = min(2, extent_count)
    copied = bytearray()
    for index in range(direct_count):
        extent_offset = 64 + index * 32
        physical = struct.unpack_from("<Q", record, extent_offset + 8)[0]
        blocks = struct.unpack_from("<Q", record, extent_offset + 16)[0]
        if not blocks or physical * B + blocks * B > len(image):
            return False
        copied.extend(image[physical * B:physical * B + blocks * B])
    return bytes(copied[:len(payload)]) == payload

def directory_entry(record_id, name):
    encoded = name.encode("utf-8")
    size = (32 + len(encoded) + 7) & ~7
    entry = bytearray(size)
    w64(entry, 0, record_id)
    w64(entry, 8, len(encoded))
    w64(entry, 16, 1)
    entry[32:32 + len(encoded)] = encoded
    checksum(entry, 24, size)
    return entry

SYSTEM_FILES = (
    (8, "sprout"), (11, "shoot"), (10, "clear"), (13, "copy"),
    (14, "say"), (15, "uptime"), (12, "list"), (16, "locate"),
    (21, "move"), (22, "plant"), (23, "read"), (24, "remove"),
    (25, "version"), (26, "where"), (17, "ping"), (18, "resolve"),
    (19, "fetch"), (20, "network"), (27, "shutdown"), (28, "reboot"),
    (29, "power"),
)

NETWORK_RECORD_ID = 30
NETWORK_CONFIG_RECORD_ID = 31
DEFAULT_NETWORK_CONFIG = (
    b"// Mangrove network configuration\n"
    b"\n"
    b"mode=dhcp\n"
)


def main():
    if len(sys.argv) != 23:
        raise SystemExit("usage: populate_mgfs.py <image> "
                         "<sprout-elf> <shoot-elf> <clear-elf> <copy-elf> "
                         "<say-elf> <uptime-elf> <list-elf> <locate-elf> "
                         "<move-elf> <plant-elf> <read-elf> <remove-elf> "
                         "<version-elf> <where-elf> <ping-elf> <resolve-elf> "
                         "<fetch-elf> <network-elf> <shutdown-elf> "
                         "<reboot-elf> <power-elf>")

    image_path = sys.argv[1]
    payloads = {
        record_id: open(path, "rb").read()
        for (record_id, _), path in zip(SYSTEM_FILES, sys.argv[2:])
    }
    image = bytearray(open(image_path, "rb").read())
    if image[:8] != MAGIC:
        raise SystemExit("populate_mgfs: not an MGFS v1 image")

    total_blocks = struct.unpack_from("<Q", image, 40)[0]
    alloc_start = struct.unpack_from("<Q", image, 104)[0]
    record_bitmap_start = struct.unpack_from("<Q", image, 120)[0]
    table_start = struct.unpack_from("<Q", image, 136)[0]
    data_start = struct.unpack_from("<Q", image, 160)[0]
    record_count = struct.unpack_from("<Q", image, 152)[0]
    if table_start != 3 or data_start <= table_start or total_blocks * B != len(image):
        raise SystemExit("populate_mgfs: unsupported MGFS geometry")
    if max(record_id for record_id, _ in SYSTEM_FILES) >= record_count:
        raise SystemExit("populate_mgfs: image has insufficient record slots")

    names = ["bin", "boot", "core", "mount", "temp", "user"]
    root_data = b"".join(directory_entry(index + 2, name)
                           for index, name in enumerate(names))
    bin_data = b"".join(directory_entry(record_id, name)
                         for record_id, name in SYSTEM_FILES)
    core_data = (directory_entry(9, "sprout.txt") +
                 directory_entry(NETWORK_RECORD_ID, "network"))
    network_data = directory_entry(NETWORK_CONFIG_RECORD_ID, "config")
    test_data = b"Mangrove handle I/O read succeeded\n"
    blocks_needed = 5 + math.ceil(len(DEFAULT_NETWORK_CONFIG) / B) + \
        sum(math.ceil(len(payload) / B) for payload in payloads.values())
    first_block = data_start
    if first_block + blocks_needed >= total_blocks:
        raise SystemExit("populate_mgfs: image has insufficient data blocks")

    def write_block(block, data):
        image[block * B:(block + 1) * B] = data[:B].ljust(B, b"\0")

    write_block(first_block, root_data)
    write_block(first_block + 1, bin_data)
    write_block(first_block + 2, core_data)
    write_block(first_block + 3, test_data)
    write_block(first_block + 4, network_data)

    file_extents = {}
    network_config_blocks = math.ceil(len(DEFAULT_NETWORK_CONFIG) / B)
    for index in range(network_config_blocks):
        write_block(first_block + 5 + index,
                    DEFAULT_NETWORK_CONFIG[index * B:(index + 1) * B])
    next_block = first_block + 5 + network_config_blocks
    for record_id, _ in SYSTEM_FILES:
        payload = payloads[record_id]
        block_count = math.ceil(len(payload) / B)
        for index in range(block_count):
            write_block(next_block + index, payload[index * B:(index + 1) * B])
        file_extents[record_id] = [(0, next_block, block_count, 1)]
        next_block += block_count

    records = [
        make_record(1, 2, len(root_data), [(0, first_block, 1, 2)]),
        make_record(2, 2, len(bin_data), [(0, first_block + 1, 1, 2)]),
        make_record(3, 2),
        make_record(4, 2, len(core_data), [(0, first_block + 2, 1, 2)]),
        make_record(5, 2), make_record(6, 2), make_record(7, 2),
        make_record(8, 1, len(payloads[8]), file_extents[8]),
        make_record(9, 1, len(test_data), [(0, first_block + 3, 1, 1)]),
    ]
    for record_id in range(10, max(record_id for record_id, _ in SYSTEM_FILES) + 1):
        if record_id in payloads:
            records.append(make_record(record_id, 1, len(payloads[record_id]),
                                       file_extents[record_id]))
        else:
            records.append(make_record(record_id, 0))
    records.append(make_record(NETWORK_RECORD_ID, 2, len(network_data),
                               [(0, first_block + 4, 1, 2)]))
    records.append(make_record(NETWORK_CONFIG_RECORD_ID, 1,
                               len(DEFAULT_NETWORK_CONFIG),
                               [(0, first_block + 5, network_config_blocks, 1)]))

    table_blocks = {}
    for slot, record in enumerate(records):
        block_index = slot // 21
        table = table_blocks.setdefault(
            block_index,
            bytearray(image[(table_start + block_index) * B:
                            (table_start + block_index + 1) * B]))
        table_offset = 24 + (slot % 21) * 192
        table[table_offset:table_offset + 192] = record
    for block_index, table in table_blocks.items():
        checksum(table, 16, B)
        image[(table_start + block_index) * B:
              (table_start + block_index + 1) * B] = table

    record_bitmap = bytearray(image[record_bitmap_start * B:(record_bitmap_start + 1) * B])
    for slot in range(len(records)):
        record_bitmap[24 + slot // 8] |= 1 << (slot % 8)
    checksum(record_bitmap, 16, B)
    image[record_bitmap_start * B:(record_bitmap_start + 1) * B] = record_bitmap

    allocation = bytearray(image[alloc_start * B:(alloc_start + 1) * B])
    for block in range(first_block, next_block):
        relative = block - data_start
        allocation[24 + relative // 8] |= 1 << (relative % 8)
    checksum(allocation, 16, B)
    image[alloc_start * B:(alloc_start + 1) * B] = allocation

    superblock = bytearray(image[:B])
    w64(superblock, 96, NETWORK_CONFIG_RECORD_ID + 1)
    checksum(superblock, 192, 200)
    image[:B] = superblock
    with open(image_path, "wb") as output:
        output.write(image)

if __name__ == "__main__":
    main()
