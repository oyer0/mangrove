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
    record_offset = table_start * B + 24 + (record_id - 1) * 192
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

def main():
    if len(sys.argv) != 6:
        raise SystemExit("usage: populate_mgfs.py <image> <sprout-elf> <shoot-elf> <hello-elf> <fstest-elf>")

    image_path, elf_path, shoot_path, hello_path, fstest_path = sys.argv[1:]
    image = bytearray(open(image_path, "rb").read())
    sprout = open(elf_path, "rb").read()
    shoot = open(shoot_path, "rb").read()
    hello = open(hello_path, "rb").read()
    fstest = open(fstest_path, "rb").read()
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

    # Population is idempotent for persistent images only when the installed
    # system payloads already match the current build outputs.  This prevents
    # an older /bin/hello from surviving a userspace rebuild.
    root = bytearray(image[data_start * B:(data_start + 1) * B])
    if (b"sprout.txt" in image and b"shoot" in image and b"hello" in image and
            record_payload_matches(image, table_start, 8, sprout) and
            record_payload_matches(image, table_start, 10, hello) and
            record_payload_matches(image, table_start, 11, shoot) and
            record_payload_matches(image, table_start, 12, fstest)):
        return

    names = ["bin", "boot", "core", "mount", "temp", "user"]
    root_data = b"".join(directory_entry(index + 2, name) for index, name in enumerate(names))
    bin_data = directory_entry(8, "sprout")
    bin_shoot_data = directory_entry(11, "shoot")
    bin_hello_data = directory_entry(10, "hello")
    bin_fstest_data = directory_entry(12, "fstest")
    core_data = directory_entry(9, "sprout.txt")
    test_data = b"Mangrove handle I/O read succeeded\n"
    blocks_needed = 4 + math.ceil(len(sprout) / B) + math.ceil(len(shoot) / B) + math.ceil(len(hello) / B) + math.ceil(len(fstest) / B)
    first_block = data_start
    if first_block + blocks_needed >= total_blocks:
        raise SystemExit("populate_mgfs: image has insufficient data blocks")

    def write_block(block, data):
        image[block * B:(block + 1) * B] = data[:B].ljust(B, b"\0")

    write_block(first_block, root_data)
    write_block(first_block + 1, bin_data)
    write_block(first_block + 2, core_data)
    write_block(first_block + 3, test_data)
    for i in range(math.ceil(len(sprout) / B)):
        write_block(first_block + 4 + i, sprout[i * B:(i + 1) * B])
    shoot_dir_block = first_block + 4 + math.ceil(len(sprout) / B)
    for i in range(math.ceil(len(shoot) / B)):
        write_block(shoot_dir_block + i, shoot[i * B:(i + 1) * B])
    hello_dir_block = shoot_dir_block + math.ceil(len(shoot) / B)
    fstest_dir_block = hello_dir_block + math.ceil(len(hello) / B)
    write_block(first_block + 1, bin_data + bin_shoot_data + bin_hello_data + bin_fstest_data)
    for i in range(math.ceil(len(hello) / B)):
        write_block(hello_dir_block + i, hello[i * B:(i + 1) * B])
    for i in range(math.ceil(len(fstest) / B)):
        write_block(fstest_dir_block + i, fstest[i * B:(i + 1) * B])

    records = [
        make_record(1, 2, len(root_data), [(0, first_block, 1, 2)]),
        make_record(2, 2, len(bin_data) + len(bin_shoot_data) + len(bin_hello_data) + len(bin_fstest_data), [(0, first_block + 1, 1, 2)]),
        make_record(3, 2),
        make_record(4, 2, len(core_data), [(0, first_block + 2, 1, 2)]),
        make_record(5, 2),
        make_record(6, 2), make_record(7, 2),
        make_record(8, 1, len(sprout), [(0, first_block + 4, math.ceil(len(sprout) / B), 1)]),
        make_record(9, 1, len(test_data), [(0, first_block + 3, 1, 1)]),
        make_record(10, 1, len(hello), [(0, hello_dir_block, math.ceil(len(hello) / B), 1)]),
        make_record(11, 1, len(shoot), [(0, shoot_dir_block, math.ceil(len(shoot) / B), 1)]),
        make_record(12, 1, len(fstest), [(0, fstest_dir_block, math.ceil(len(fstest) / B), 1)]),
    ]
    table = bytearray(image[table_start * B:(table_start + 1) * B])
    for slot, record in enumerate(records):
        table[24 + slot * 192:24 + (slot + 1) * 192] = record
    checksum(table, 16, B)
    image[table_start * B:(table_start + 1) * B] = table

    record_bitmap = bytearray(image[record_bitmap_start * B:(record_bitmap_start + 1) * B])
    for slot in range(len(records)):
        record_bitmap[24 + slot // 8] |= 1 << (slot % 8)
    checksum(record_bitmap, 16, B)
    image[record_bitmap_start * B:(record_bitmap_start + 1) * B] = record_bitmap

    allocation = bytearray(image[alloc_start * B:(alloc_start + 1) * B])
    for block in range(first_block, first_block + blocks_needed):
        relative = block - data_start
        allocation[24 + relative // 8] |= 1 << (relative % 8)
    checksum(allocation, 16, B)
    image[alloc_start * B:(alloc_start + 1) * B] = allocation

    superblock = bytearray(image[:B])
    w64(superblock, 96, 13)
    checksum(superblock, 192, 200)
    image[:B] = superblock
    with open(image_path, "wb") as output:
        output.write(image)

if __name__ == "__main__":
    main()
