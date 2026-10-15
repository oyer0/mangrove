# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded independent fixture encoding; never services runtime filesystem I/O."""
import mmap
import os
from pathlib import Path
import shutil
import struct
import subprocess


class Unavailable(RuntimeError):
    pass


def external_tools(backend):
    suffix = "fat" if backend == "fat32" else "exfat"
    search = os.environ.get("PATH", "") + os.pathsep + "/usr/sbin"
    result = {key: shutil.which(name, path=search) for key, name in
              (("formatter", "mkfs." + suffix), ("checker", "fsck." + suffix))}
    missing = [key for key, value in result.items() if not value]
    if missing:
        raise Unavailable(backend + " requires independent " + ", ".join(missing) + " tools")
    return result


def initial_files(layout):
    if layout in ("empty", "preallocated-directory"):
        return {}
    return {"/old.txt": b"seed" + bytes(7996 if layout == "short-valid" else 0)}


def exfat_checksum(data):
    value = 0
    for index, byte in enumerate(data):
        if index not in (2, 3):
            value = (((value >> 1) | ((value & 1) << 15)) + byte) & 65535
    return value


def exfat_fixture(data, layout):
    get32 = lambda at: struct.unpack_from("<I", data, at)[0]
    fat, heap, count, root = (get32(at) for at in (80, 88, 92, 96))
    cb = 1 << (data[108] + data[109])
    assert cb == 4096 and data[110] == 1 and count < 32768
    address = lambda cluster: heap * 512 + (cluster - 2) * cb
    root_at = address(root)
    bitmap = end = None
    for offset in range(0, cb, 32):
        at = root_at + offset
        if data[at] == 0x81:
            bitmap = address(get32(at + 20))
            assert struct.unpack_from("<Q", data, at + 24)[0] <= cb
        if data[at] == 0:
            end = at
            break
    assert bitmap is not None and end is not None and end + 128 <= root_at + cb
    free = [c for c in range(2, count + 2)
            if not data[bitmap + (c - 2) // 8] & (1 << ((c - 2) % 8))]
    for cluster in free:
        data[address(cluster):address(cluster) + cb] = b"\x5a" * cb
    if layout != "empty":
        length = 8000 if layout == "short-valid" else 4
        chained = layout in ("chained-slack", "short-valid")
        selected = free[:4:2] if length == 8000 else free[:1]
        for i, cluster in enumerate(selected):
            data[bitmap + (cluster - 2) // 8] |= 1 << ((cluster - 2) % 8)
            at = address(cluster)
            data[at:at + cb] = b"\xa5" * cb
            if i == 0:
                data[at:at + 4] = b"seed"
            if chained:
                struct.pack_into("<I", data, fat * 512 + cluster * 4,
                                 selected[i + 1] if i + 1 < len(selected) else 0xffffffff)
        entry = bytearray(96)
        entry[0:2] = b"\x85\x02"
        entry[4] = 0x20
        timestamp = (20 << 25) | (1 << 21) | (1 << 16)  # UTC 2000-01-01.
        for at in (8, 12, 16):
            struct.pack_into("<I", entry, at, timestamp)
        entry[22:25] = b"\x80" * 3  # Valid UTC offsets, 0 minutes.
        entry[32:36] = bytes((0xc0, 1 if chained else 3, 0, 7))
        struct.pack_into("<Q", entry, 40, 4)
        struct.pack_into("<I", entry, 52, selected[0])
        struct.pack_into("<Q", entry, 56, length)
        entry[64] = 0xc1
        hashed = 0
        for i, char in enumerate("old.txt"):
            # Independent ASCII uppercase hash, no production upcase routine.
            for byte in (ord(char.upper()), 0):
                hashed = (((hashed >> 1) | ((hashed & 1) << 15)) + byte) & 65535
            struct.pack_into("<H", entry, 66 + 2 * i, ord(char))
        struct.pack_into("<H", entry, 36, hashed)
        struct.pack_into("<H", entry, 2, exfat_checksum(entry))
        data[end:end + 96] = entry
        data[end + 96:end + 128] = bytes(32)
    # Normalize only formatter nondeterminism/fixture utilization. Mutable
    # utilization is explicitly unknown; flags/timestamps are not weakened.
    for base in (0, 12 * 512):
        struct.pack_into("<I", data, base + 100, 0x12345678)
        data[base + 112] = 0xff
        checksum = 0
        for index in range(11 * 512):
            if index not in (106, 107, 112):
                checksum = (((checksum >> 1) | ((checksum & 1) << 31)) + data[base + index]) & 0xffffffff
        data[base + 11 * 512:base + 12 * 512] = struct.pack("<I", checksum) * 128


def fat32_fixture(data, layout):
    get16 = lambda at: struct.unpack_from("<H", data, at)[0]
    get32 = lambda at: struct.unpack_from("<I", data, at)[0]
    reserved, fats, fat_size, root = get16(14), data[16], get32(36), get32(44)
    cb = data[13] * 512
    heap = reserved + fats * fat_size
    count = (get32(32) - heap) // data[13]
    assert get16(11) == 512 and cb == 512 and fats == 2 and count >= 65525
    address = lambda cluster: heap * 512 + (cluster - 2) * cb
    free = []
    for cluster in range(2, count + 2):
        if get32(reserved * 512 + cluster * 4) & 0x0fffffff == 0:
            free.append(cluster)
            data[address(cluster):address(cluster) + cb] = b"\x5a" * cb
    if layout != "empty":
        assert layout == "slack"
        cluster = free[0]
        for fat in range(fats):
            struct.pack_into("<I", data, (reserved + fat * fat_size) * 512 + cluster * 4, 0x0fffffff)
        entry = bytearray(32)
        entry[:11] = b"OLD     TXT"
        entry[11:13] = b"\x20\x18"  # Lowercase basename/extension presentation.
        date = (20 << 9) | (1 << 5) | 1
        for at in (16, 18, 24):
            struct.pack_into("<H", entry, at, date)
        struct.pack_into("<H", entry, 20, cluster >> 16)
        struct.pack_into("<H", entry, 26, cluster & 65535)
        struct.pack_into("<I", entry, 28, 4)
        root_at = address(root)
        slot = next(at for at in range(root_at, root_at + cb, 32) if data[at] == 0)
        assert slot + 64 <= root_at + cb
        data[slot:slot + 32] = entry
        data[slot + 32:slot + 64] = bytes(32)
        at = address(cluster)
        data[at:at + cb] = b"\xa5" * cb
        data[at:at + 4] = b"seed"
    # Accurate initial FSInfo counters in both copies. Runtime may legitimately
    # invalidate these advisory hints; checker warnings remain in the report.
    fsinfo, backup = get16(48), get16(50)
    for sector in (fsinfo, backup + fsinfo):
        struct.pack_into("<II", data, sector * 512 + 488,
                         len(free) - (layout != "empty"), free[layout != "empty"])


def create(path, backend, layout, tools, mgfs_arguments):
    path = Path(path)
    if path.exists():
        raise ValueError("fixture path already exists")
    if backend == "mgfs":
        assert layout in ("empty", "preallocated-directory")
        command = [str(tools["mkmgfs"]), *mgfs_arguments, str(path)]
    else:
        size = 64 * 1024 * 1024 if backend == "fat32" else 12000 * 512
        with path.open("xb") as stream:
            stream.truncate(size)
        command = ([tools["formatter"], "--invariant", "-F", "32", "-s", "1",
                    "-i", "12345678", str(path)] if backend == "fat32" else
                   [tools["formatter"], "-c", "4096", str(path)])
    formatted = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
    fixture = {}
    if backend == "mgfs" and layout == "preallocated-directory":
        from mgfs_images import preallocate
        data = bytearray(path.read_bytes())
        fixture = preallocate(data, logical_size=4080)
        path.write_bytes(data)
    if backend != "mgfs":
        with path.open("r+b") as stream, mmap.mmap(stream.fileno(), 0) as data:
            (fat32_fixture if backend == "fat32" else exfat_fixture)(data, layout)
            data.flush()
    return dict(command=command[:-1], stdout=formatted.stdout, stderr=formatted.stderr,
                layout=layout, directory_fixture=fixture, poison_free_byte=0x5a if backend != "mgfs" else None,
                poison_allocated_slack_byte=0xa5 if layout != "empty" else None,
                imported_valid_length=4 if initial_files(layout) else None)


def check(path, backend, tools):
    command = ([str(tools["mgfsck"]), str(path)] if backend == "mgfs" else
               [tools["checker"], "-n", str(path)])
    result = subprocess.run(command, text=True, capture_output=True, timeout=30)
    return dict(command=command, returncode=result.returncode,
                stdout=result.stdout, stderr=result.stderr)
