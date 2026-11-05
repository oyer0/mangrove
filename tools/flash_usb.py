#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Flash a compact Mangrove image to an explicitly selected Linux USB disk.

The CLI accepts block devices only. Tests use regular-file transports through
the core functions; no CLI switch bypasses device admission or confirmation.
"""
import argparse
from dataclasses import dataclass
import fcntl
import hashlib
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

SECTOR = 512
COMPACT_SECTORS = 264225
COMPACT_BYTES = COMPACT_SECTORS * SECTOR
ARRAY_BYTES = 128 * 128
ARRAY_SECTORS = ARRAY_BYTES // SECTOR
CHUNK = 1024 * 1024
ESP_TYPE = bytes.fromhex('28732ac11ff8d211ba4b00a0c93ec93b')
ROOT = Path(__file__).resolve().parents[1]


class FlashError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise FlashError(message)


def read_exact(fd, offset, size):
    data = bytearray()
    while len(data) < size:
        part = os.pread(fd, size - len(data), offset + len(data))
        require(part, f'short read at byte {offset + len(data)}')
        data.extend(part)
    return bytes(data)


def write_exact(fd, offset, data):
    done = 0
    while done < len(data):
        count = os.pwrite(fd, data[done:], offset + done)
        require(count > 0, f'zero-length write at byte {offset + done}')
        done += count


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def u64(data, offset):
    return struct.unpack_from('<Q', data, offset)[0]


def seal_header(header):
    result = bytearray(header)
    struct.pack_into('<I', result, 16, 0)
    struct.pack_into('<I', result, 16, zlib.crc32(result[:92]))
    return bytes(result)


@dataclass(frozen=True)
class GPT:
    mbr: bytes
    primary: bytes
    backup: bytes
    entries: bytes
    sectors: int
    partitions: tuple


def load_gpt(read, size, compact=False):
    """Validate both complete copies and the supported fixed Mangrove layout."""
    require(size >= COMPACT_BYTES and size % SECTOR == 0,
            'disk capacity is too small or not sector aligned')
    if compact:
        require(size == COMPACT_BYTES,
                'input must be the compact 135283200-byte Mangrove image; rebuild without USB_IMAGE_SECTORS')
    sectors = size // SECTOR
    mbr, primary, backup = read(0, SECTOR), read(SECTOR, SECTOR), read(size-SECTOR, SECTOR)
    require(mbr[510:512] == b'\x55\xaa' and mbr[450] == 0xee and
            u32(mbr, 454) == 1 and u32(mbr, 458) == min(sectors-1, 0xffffffff) and
            mbr[462:510] == bytes(48), 'invalid or hybrid protective MBR')
    arrays = []
    for header, current, alternate, entries_lba in (
            (primary, 1, sectors-1, 2),
            (backup, sectors-1, 1, sectors-1-ARRAY_SECTORS)):
        require(header[:8] == b'EFI PART' and u32(header, 8) == 0x10000 and
                u32(header, 12) == 92 and header[20:24] == bytes(4) and
                header[92:] == bytes(SECTOR-92), 'unsupported GPT header')
        require(seal_header(header) == header, 'GPT header CRC mismatch')
        require(u64(header, 24) == current and u64(header, 32) == alternate and
                u64(header, 72) == entries_lba, 'GPT header locations do not match disk capacity')
        require(u64(header, 40) == 34 and u64(header, 48) == sectors-34 and
                header[56:72] != bytes(16) and u32(header, 80) == 128 and
                u32(header, 84) == 128, 'unsupported GPT geometry')
        array = read(entries_lba * SECTOR, ARRAY_BYTES)
        require(zlib.crc32(array) == u32(header, 88), 'GPT entry-array CRC mismatch')
        arrays.append(array)
    require(primary[40:72] == backup[40:72] and arrays[0] == arrays[1] and
            primary[80:92] == backup[80:92], 'GPT copies disagree')
    partitions = []
    guids = set()
    for index in range(128):
        entry = arrays[0][index*128:(index+1)*128]
        if entry[:16] == bytes(16):
            require(entry == bytes(128), 'unsupported nonzero unused GPT entry')
            continue
        guid = entry[16:32]
        require(guid != bytes(16) and guid not in guids, 'invalid or duplicate partition GUID')
        guids.add(guid)
        first, last = u64(entry, 32), u64(entry, 40)
        require(34 <= first <= last <= sectors-34, 'partition outside usable space')
        require(all(last < p[1] or first > p[2] for p in partitions), 'overlapping partitions')
        try:
            name = entry[56:128].decode('utf-16-le').rstrip('\0')
        except UnicodeDecodeError as error:
            raise FlashError('invalid GPT partition name') from error
        partitions.append((name, first, last, entry[:16]))
    require(tuple(p[:3] for p in partitions) == (
        ('MANGROVE_ESP', 2048, 133119), ('MANGROVE_ROOT', 133120, 264191)),
        'unsupported Mangrove partition layout')
    require(partitions[0][3] == ESP_TYPE and partitions[1][3] != ESP_TYPE,
            'incorrect ESP/root partition types')
    return GPT(mbr, primary, backup, arrays[0], sectors, tuple(partitions))


@dataclass(frozen=True)
class Plan:
    size: int
    patches: tuple


def relocation_plan(gpt, size, logical=512, physical=512, alignment=0):
    require(logical == SECTOR and physical >= logical and
            physical & (physical-1) == 0 and physical <= 4096 and alignment == 0,
            'unsupported logical/physical sector size or device alignment')
    require(size >= COMPACT_BYTES and size % SECTOR == 0 and size <= (1 << 63)-1,
            'destination capacity is insufficient or unaligned')
    require(all(first*SECTOR % physical == 0 for _, first, _, _ in gpt.partitions),
            'partition starts are not physically aligned')
    sectors = size // SECTOR
    mbr = bytearray(gpt.mbr)
    struct.pack_into('<I', mbr, 458, min(sectors-1, 0xffffffff))
    mbr[451:454] = b'\xff'*3  # conventional protective MBR CHS end marker
    primary, backup = bytearray(gpt.primary), bytearray(gpt.backup)
    tail_array = sectors-1-ARRAY_SECTORS
    for header, current, other, array in ((primary, 1, sectors-1, 2),
                                          (backup, sectors-1, 1, tail_array)):
        struct.pack_into('<QQ', header, 24, current, other)
        struct.pack_into('<Q', header, 48, sectors-34)
        struct.pack_into('<Q', header, 72, array)
        struct.pack_into('<I', header, 88, zlib.crc32(gpt.entries))
    return Plan(size, (
        (0, bytes(mbr)), (SECTOR, seal_header(primary)),
        # Clear the old copy before writing the new complete metadata, even
        # when the old/new ranges overlap on a disk only one sector larger.
        ((COMPACT_SECTORS-33)*SECTOR, bytes(33*SECTOR)),
        (tail_array*SECTOR, gpt.entries), ((sectors-1)*SECTOR, seal_header(backup))))


class FDTarget:
    def __init__(self, fd, block=False):
        self.fd, self.block = fd, block
        self.written = self.read_bytes = 0

    def write(self, offset, data):
        write_exact(self.fd, offset, data)
        self.written += len(data)

    def read(self, offset, size):
        data = read_exact(self.fd, offset, size)
        self.read_bytes += len(data)
        return data

    def sync(self, invalidate=False):
        os.fsync(self.fd)
        if invalidate and self.block:
            # BLKFLSBUF: never claim uncached verification if it is refused.
            fcntl.ioctl(self.fd, 0x1261)


def flash_snapshot(source_fd, target, gpt, plan, recheck, confirm, progress=print):
    """One frozen image, one pinned target, and explicit admission callbacks."""
    recheck()
    require(confirm(), 'confirmation declined; destination was not written')
    recheck()
    progress(f'Writing {COMPACT_BYTES} image bytes and bounded GPT metadata...')
    for offset in range(0, COMPACT_BYTES, CHUNK):
        target.write(offset, read_exact(source_fd, offset, min(CHUNK, COMPACT_BYTES-offset)))
    # Relocate the backup completely before publishing the new primary.
    for offset, data in plan.patches[2:]:
        target.write(offset, data)
    target.sync()
    for offset, data in plan.patches[:2]:
        target.write(offset, data)
    target.sync(invalidate=True)
    progress('Verifying the compact region, partition bytes, and relocated GPT...')
    for offset in range(0, COMPACT_BYTES, CHUNK):
        size = min(CHUNK, COMPACT_BYTES-offset)
        expected = bytearray(read_exact(source_fd, offset, size))
        for start, data in plan.patches:
            lo, hi = max(offset, start), min(offset+size, start+len(data))
            if lo < hi:
                expected[lo-offset:hi-offset] = data[lo-start:hi-start]
        require(target.read(offset, size) == expected,
                f'destination verification mismatch at byte {offset}')
    for offset, data in plan.patches[3:]:
        require(target.read(offset, len(data)) == data, 'relocated GPT readback mismatch')
    observed = load_gpt(target.read, plan.size)
    require(observed.entries == gpt.entries and
            observed.primary[56:72] == gpt.primary[56:72], 'GPT identities changed')
    recheck()
    return target.written, target.read_bytes


def snapshot_image(path, folder, checker):
    require(stat.S_ISREG(path.lstat().st_mode), 'image must be a regular file, not a symlink/device')
    require(checker.is_file() and os.access(checker, os.X_OK),
            f'MGFS checker missing: build it without sudo, or pass --mgfsck ({checker})')
    snapshot = folder/'image.img'
    digest = hashlib.sha256()
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and before.st_size == COMPACT_BYTES,
                'input must be the compact 135283200-byte Mangrove image')
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        with snapshot.open('xb') as output:
            for offset in range(0, COMPACT_BYTES, CHUNK):
                data = read_exact(fd, offset, min(CHUNK, COMPACT_BYTES-offset))
                output.write(data); digest.update(data)
        after = os.fstat(fd)
        require((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'source changed during snapshot')
    finally:
        os.close(fd)
    with snapshot.open('rb') as source:
        gpt = load_gpt(lambda o,n: read_exact(source.fileno(), o,n), COMPACT_BYTES, compact=True)
        root = folder/'root.img'
        with root.open('xb') as output:
            for offset in range(133120*SECTOR, 264192*SECTOR, CHUNK):
                output.write(read_exact(source.fileno(), offset, CHUNK))
    with (folder/'mgfsck.log').open('w+b') as log:
        result = subprocess.run([str(checker), str(root)], stdout=log,
                                stderr=subprocess.STDOUT, timeout=120, check=False)
        log.seek(0)
        summary = log.read(65536).decode(errors='replace')
    require(result.returncode == 0 and 'Errors: 0\n' in summary and
            'Warnings: 0\n' in summary, f'MGFS root validation failed:\n{summary}')
    return snapshot, gpt, digest.hexdigest()


def unescape(text):
    return re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), text)


def dev_id(value):
    return f'{os.major(value)}:{os.minor(value)}'


def backing_disks(device, sys_root, seen=None):
    seen = set() if seen is None else seen
    require(device not in seen, 'cycle in block-device ancestry')
    node = (sys_root/'dev/block'/device).resolve(strict=True)
    seen = seen | {device}
    if (node/'partition').exists():
        return backing_disks((node.parent/'dev').read_text().strip(), sys_root, seen)
    slaves = list((node/'slaves').iterdir())
    if slaves:
        disks = set()
        for slave in slaves:
            disks |= backing_disks((slave/'dev').read_text().strip(), sys_root, seen)
        return disks
    return {device}


@dataclass(frozen=True)
class Device:
    path: Path
    number: int
    sys_path: Path
    sys_inode: int
    usb_inode: int
    vendor_id: str
    product_id: str
    serial: str
    model: str


def inspect_device(path, expect_model=None, sys_root=Path('/sys'), proc_root=Path('/proc'),
                   stat_path=os.stat):
    path = path.resolve(strict=True)
    info = stat_path(path)
    require(stat.S_ISBLK(info.st_mode), 'destination must be a whole block device')
    number = dev_id(info.st_rdev)
    node = (sys_root/'dev/block'/number).resolve(strict=True)
    require(not (node/'partition').exists(), 'select a whole USB disk, not a partition')
    require((node/'dev').read_text().strip() == number, 'sysfs/device identity mismatch')
    require((node/'removable').read_text().strip() == '1', 'destination is not removable')
    require((node/'ro').read_text().strip() == '0', 'destination is read-only')
    usb = next((p for p in node.parents if (p/'idVendor').is_file() and
                (p/'idProduct').is_file() and
                (p/'subsystem').resolve().name == 'usb'), None)
    require(usb is not None, 'destination has no USB-device ancestry')
    model = (node/'device/model').read_text().strip()
    require(expect_model is None or model == expect_model,
            f'device model mismatch: expected {expect_model!r}, found {model!r}')
    children = {number}
    members = [node] + [p for p in node.iterdir() if (p/'partition').is_file()]
    for part in members:
        children.add((part/'dev').read_text().strip())
        require(not list((part/'holders').iterdir()), 'destination or a partition has active holders')
    protected = set()
    root_resolved = False
    for line in (proc_root/'self/mountinfo').read_text().splitlines():
        fields = line.split()
        require(len(fields) >= 10 and '-' in fields, 'malformed mount inventory')
        separator = fields.index('-')
        require(separator >= 6 and separator+3 < len(fields), 'malformed mount inventory')
        mounted, mountpoint, source = fields[2], unescape(fields[4]), unescape(fields[separator+2])
        if not (sys_root/'dev/block'/mounted).exists() and source.startswith('/dev/'):
            source_info = stat_path(source)
            require(stat.S_ISBLK(source_info.st_mode), 'unresolved block mount source')
            mounted = dev_id(source_info.st_rdev)
        require(mounted not in children, f'destination is mounted at {mountpoint}; unmount it manually')
        if (sys_root/'dev/block'/mounted).exists():
            protected |= backing_disks(mounted, sys_root)
            if mountpoint == '/': root_resolved = True
    # Anonymous Btrfs mount IDs can describe multiple physical members.
    btrfs = sys_root/'fs/btrfs'
    if btrfs.exists():
        for member in btrfs.glob('*/devices/*/dev'):
            protected |= backing_disks(member.read_text().strip(), sys_root)
    require(root_resolved, 'cannot establish the host root disk; refusing this environment')
    require(number not in protected, 'destination backs a mounted/system filesystem')
    lines = (proc_root/'swaps').read_text().splitlines()
    require(lines and lines[0].startswith('Filename'), 'cannot inspect active swap')
    for line in lines[1:]:
        swap = stat_path(unescape(line.split()[0]))
        swap_id = dev_id(swap.st_rdev if stat.S_ISBLK(swap.st_mode) else swap.st_dev)
        require(swap_id not in children, 'destination contains active swap')
    serial = (usb/'serial').read_text().strip() if (usb/'serial').exists() else ''
    vendor, product = (usb/'idVendor').read_text().strip(), (usb/'idProduct').read_text().strip()
    require(model and len(model) <= 128 and len(serial) <= 256 and
            re.fullmatch(r'[0-9a-fA-F]{4}', vendor) and
            re.fullmatch(r'[0-9a-fA-F]{4}', product) and
            all(c.isprintable() for c in model+serial), 'invalid device identity text')
    return Device(path, info.st_rdev, node, node.stat().st_ino, usb.stat().st_ino,
                  vendor, product, serial, model)


def geometry(fd):
    def query(command, fmt):
        return struct.unpack(fmt, fcntl.ioctl(fd, command, bytes(struct.calcsize(fmt))))[0]
    require(query(0x125e, '=I') == 0, 'block device is read-only')
    return (query(0x80081272, '=Q'), query(0x1268, '=I'),
            query(0x127b, '=I'), query(0x127a, '=i'))


def confirm_device(device):
    token = f'ERASE {device.path} {device.serial or dev_id(device.number)}'
    # Buffered r+ uses BufferedRandom, which requires seeking. A terminal is
    # sequential: use separate input/output streams, independent of stdin.
    with open('/dev/tty', 'r', encoding='utf-8') as reader, \
         open('/dev/tty', 'w', encoding='utf-8') as writer:
        require(reader.isatty() and writer.isatty(),
                'interactive confirmation requires a terminal')
        writer.write(f'Type exactly {token!r} to overwrite this USB: '); writer.flush()
        return reader.readline().rstrip('\n') == token


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--device', type=Path, required=True)
    parser.add_argument('--expect-model', help='require this exact SCSI model, e.g. Cruzer Blade')
    parser.add_argument('--mgfsck', type=Path, default=ROOT/'build/mgfsck')
    args = parser.parse_args(argv)
    try:
        require(sys.platform.startswith('linux'), 'physical flashing is supported on Linux only')
        scratch = Path(os.environ.get('TMPDIR', ROOT/'build/flash-usb-scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='flash-', dir=scratch) as temporary:
            snapshot, gpt, digest = snapshot_image(args.image, Path(temporary), args.mgfsck.resolve())
            device = inspect_device(args.device, args.expect_model)
            fd = os.open(device.path, os.O_RDWR | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                require(stat.S_ISBLK(os.fstat(fd).st_mode) and
                        os.fstat(fd).st_rdev == device.number, 'opened device identity changed')
                dimensions = geometry(fd)
                plan = relocation_plan(gpt, *dimensions)
                def recheck():
                    require(inspect_device(args.device, args.expect_model) == device and
                            geometry(fd) == dimensions, 'device identity/capacity changed')
                print(f'Image: {args.image}\nSHA-256: {digest}\n'
                      f'Destination: {device.path} model={device.model!r} serial={device.serial!r} '
                      f'USB={device.vendor_id}:{device.product_id}\n'
                      f'Capacity: {dimensions[0]} bytes; logical/physical sectors: {dimensions[1:3]}\n'
                      'This replaces the USB partition table and image region. Unallocated space is left untouched.\n'
                      'No automatic unmount or sudo is performed.')
                with snapshot.open('rb') as source:
                    written, verified = flash_snapshot(source.fileno(), FDTarget(fd, block=True),
                        gpt, plan, recheck, lambda: confirm_device(device))
                print(f'VERIFIED: GPT, partition identities/contents, and checked MGFS root; '
                      f'{written} bytes written, {verified} bytes read back.')
            finally:
                os.close(fd)
        return 0
    except (OSError, FlashError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        print(f'flash-usb: FAILED: {error}\n'
              'No success is implied. If writing began, the USB may be incomplete; rerun after resolving the error.',
              file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
