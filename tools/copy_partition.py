#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy the GPT-marked Mangrove root partition without rewriting the disk."""

import argparse
import os
import struct
import sys
import tempfile


SECTOR_SIZE = 512
GPT_SIGNATURE = b"EFI PART"
GPT_HEADER_MIN = 92
GPT_ENTRY_MIN = 128
GPT_ENTRY_MAX = 1024
GPT_ENTRY_LIMIT = 4096
ROOT_NAME = "MANGROVE_ROOT"
COPY_CHUNK = 1024 * 1024


def fail(message):
    raise RuntimeError(message)


def read_exact(fd, offset, size):
    data = bytearray()
    while len(data) < size:
        chunk = os.pread(fd, min(COPY_CHUNK, size - len(data)), offset + len(data))
        if not chunk:
            fail("short read at byte offset %d" % (offset + len(data)))
        data.extend(chunk)
    return bytes(data)


def write_exact(fd, offset, data):
    written = 0
    while written < len(data):
        count = os.pwrite(fd, data[written:], offset + written)
        if count <= 0:
            fail("short write at byte offset %d" % (offset + written))
        written += count


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def gpt_name(entry):
    try:
        value = entry[56:128].decode("utf-16le")
    except UnicodeDecodeError:
        fail("GPT contains an invalid UTF-16 partition name")
    return value.rstrip("\x00")


def load_root_partition(disk_fd, disk_size):
    if disk_size == 0 or disk_size % SECTOR_SIZE:
        fail("disk size is not a whole number of sectors")
    disk_sectors = disk_size // SECTOR_SIZE
    header = read_exact(disk_fd, SECTOR_SIZE, SECTOR_SIZE)
    if header[:8] != GPT_SIGNATURE:
        fail("persistent disk does not contain a GPT")
    header_size = u32(header, 12)
    if header_size < GPT_HEADER_MIN or header_size > SECTOR_SIZE:
        fail("GPT header size is invalid")
    backup_lba = u64(header, 32)
    first_usable = u64(header, 40)
    last_usable = u64(header, 48)
    entries_lba = u64(header, 72)
    entry_count = u32(header, 80)
    entry_size = u32(header, 84)
    if backup_lba + 1 != disk_sectors:
        fail("disk size does not match the GPT backup LBA")
    if first_usable > last_usable or last_usable >= disk_sectors:
        fail("GPT usable-sector range is invalid")
    if (entry_count == 0 or entry_count > GPT_ENTRY_LIMIT or
            entry_size < GPT_ENTRY_MIN or entry_size > GPT_ENTRY_MAX or
            entry_size % 8 or entries_lba >= disk_sectors):
        fail("GPT partition-entry geometry is invalid")
    entry_bytes = entry_count * entry_size
    entry_end = entries_lba * SECTOR_SIZE + entry_bytes
    if entry_end > disk_size:
        fail("GPT partition-entry array exceeds the disk")

    root = None
    occupied = []
    for index in range(entry_count):
        entry = read_exact(disk_fd, entries_lba * SECTOR_SIZE + index * entry_size,
                           entry_size)
        if entry[:16] == b"\x00" * 16:
            continue
        if entry_size < 128:
            fail("GPT partition entry is truncated")
        start = u64(entry, 32)
        end = u64(entry, 40)
        if start > end or end >= disk_sectors or start < first_usable or end > last_usable:
            fail("GPT partition range is invalid")
        for other_start, other_end in occupied:
            if start <= other_end and other_start <= end:
                fail("GPT partitions overlap")
        occupied.append((start, end))
        if gpt_name(entry) == ROOT_NAME:
            if root is not None:
                fail("multiple %s partitions found" % ROOT_NAME)
            root = (start, end)
    if root is None:
        fail("GPT partition %s not found" % ROOT_NAME)
    start, end = root
    if start * SECTOR_SIZE < entry_end:
        fail("root partition overlaps GPT metadata")
    return start, end - start + 1


def copy_partition(mode, disk_path, root_path):
    disk_flags = os.O_RDWR if mode == "write" else os.O_RDONLY
    disk_fd = os.open(disk_path, disk_flags)
    try:
        disk_size = os.fstat(disk_fd).st_size
        start, sectors = load_root_partition(disk_fd, disk_size)
        partition_bytes = sectors * SECTOR_SIZE
        print("[DEV] MANGROVE_ROOT start=%d sectors=%d bytes=%d" %
              (start, sectors, partition_bytes))

        if mode == "extract":
            root_dir = os.path.dirname(os.path.abspath(root_path)) or "."
            fd, temporary = tempfile.mkstemp(prefix=".root-copy-", dir=root_dir)
            try:
                with os.fdopen(fd, "wb", closefd=True) as output:
                    source_offset = start * SECTOR_SIZE
                    for offset in range(0, partition_bytes, COPY_CHUNK):
                        size = min(COPY_CHUNK, partition_bytes - offset)
                        data = read_exact(disk_fd, source_offset + offset, size)
                        if output.write(data) != len(data):
                            fail("short write while extracting the root partition")
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, root_path)
            except Exception:
                try:
                    os.unlink(temporary)
                except FileNotFoundError:
                    pass
                raise
        else:
            root_size = os.stat(root_path).st_size
            if root_size != partition_bytes:
                fail("root image is %d bytes; expected %d" %
                     (root_size, partition_bytes))
            root_fd = os.open(root_path, os.O_RDONLY)
            try:
                destination_offset = start * SECTOR_SIZE
                for offset in range(0, partition_bytes, COPY_CHUNK):
                    size = min(COPY_CHUNK, partition_bytes - offset)
                    write_exact(disk_fd, destination_offset + offset,
                                read_exact(root_fd, offset, size))
                os.fsync(disk_fd)
            finally:
                os.close(root_fd)
    finally:
        os.close(disk_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("extract", "write"))
    parser.add_argument("--disk", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    try:
        copy_partition(args.mode, args.disk, args.root)
    except (OSError, RuntimeError, ValueError) as error:
        print("copy_partition: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
