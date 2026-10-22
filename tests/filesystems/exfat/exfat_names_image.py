# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent exFAT entry-set encoding on a fresh mkfs.exfat volume."""
import struct
from pathlib import Path

def seed_image(path):
    data = bytearray(Path(path).read_bytes())
    get32 = lambda off: struct.unpack_from('<I', data, off)[0]
    heap, root = get32(88), get32(96)
    cluster_bytes = 1 << (data[108] + data[109])
    address = lambda cluster: heap * 512 + (cluster - 2) * cluster_bytes
    root_at = address(root)
    bitmap = upcase = None
    for offset in range(0, cluster_bytes, 32):
        at = root_at + offset
        if data[at] == 0x81: bitmap = get32(at + 20)
        if data[at] == 0x82:
            upcase = get32(at + 20)
            upcase_length = struct.unpack_from('<Q', data, at + 24)[0]
        if not data[at]:
            end_offset = offset
            break
    else:
        raise AssertionError('Fresh root directory has no end marker')
    assert bitmap and upcase
    assert end_offset <= 480
    raw = data[address(upcase):address(upcase) + upcase_length]
    table = []; offset = 0
    while offset < len(raw):
        value = struct.unpack_from('<H', raw, offset)[0]; offset += 2
        if value == 0xffff and len(table) != 0xffff:
            run = struct.unpack_from('<H', raw, offset)[0]; offset += 2
            table.extend(range(len(table), len(table) + run))
        else: table.append(value)
    assert len(table) == 65536
    def mark(cluster):
        bit = cluster - 2
        data[address(bitmap) + bit // 8] |= 1 << (bit % 8)
    next_cluster = root + 1
    def allocate(payload, directory=False):
        nonlocal next_cluster
        cluster = next_cluster; next_cluster += 1; mark(cluster)
        data[address(cluster):address(cluster) + len(payload)] = payload
        if directory: struct.pack_into('<I', data, get32(80) * 512 + cluster * 4, 0xffffffff)
        return cluster
    rotate = lambda n: (n >> 1) | ((n & 1) << 15)
    def entries(name, cluster, length, directory=False):
        raw = name.encode('utf-16-le'); units = struct.unpack('<' + 'H' * (len(raw)//2), raw)
        count = 2 + (len(units) + 14)//15; result = bytearray(count * 32)
        result[0] = 0x85; result[1] = count - 1
        struct.pack_into('<H', result, 4, 0x10 if directory else 0x20)
        # Fixed fixture date: 2000-01-01 00:00:00 UTC, independently encoded
        # from the exFAT year/month/day fields, not a production clock sample.
        timestamp = ((2000 - 1980) << 25) | (1 << 21) | (1 << 16)
        for at in (8, 12, 16): struct.pack_into('<I', result, at, timestamp)
        result[20] = result[21] = 0  # Exact second; no 10-ms fraction.
        result[22:25] = b'\x80\x80\x80'  # Valid UTC offsets, each zero.
        result[32] = 0xc0; result[33] = 1 if directory else 3; result[35] = len(units)
        struct.pack_into('<Q', result, 40, length); struct.pack_into('<I', result, 52, cluster)
        struct.pack_into('<Q', result, 56, length)
        hashed = 0
        for i, unit in enumerate(units):
            upper = table[unit]
            hashed = (rotate(hashed) + (upper & 255)) & 65535
            hashed = (rotate(hashed) + (upper >> 8)) & 65535
            at = (2 + i//15) * 32; result[at] = 0xc1
            struct.pack_into('<H', result, at + 2 + 2*(i%15), unit)
        struct.pack_into('<H', result, 36, hashed)
        checksum = 0
        for i, byte in enumerate(result):
            if i not in (2, 3): checksum = (rotate(checksum) + byte) & 65535
        struct.pack_into('<H', result, 2, checksum)
        return result
    # Deleted entries make the first imported primary genuinely split-sector.
    for offset in range(end_offset, 480, 32): data[root_at + offset] = 0x05
    offset = 480
    names = ['é'*128, '界'*255, '😀'*127+'X', 'é'*127+'x', 'A'*255]
    for i, name in enumerate(names):
        payload = bytes([65+i]); cluster = allocate(payload)
        entry = entries(name, cluster, len(payload))
        data[root_at+offset:root_at+offset+len(entry)] = entry; offset += len(entry)
    child_cluster = allocate(b'Z')
    child = entries('inside.txt', child_cluster, 1)
    directory_cluster = allocate(child, True)
    entry = entries('é'*255, directory_cluster, cluster_bytes, True)
    data[root_at+offset:root_at+offset+len(entry)] = entry; offset += len(entry)
    cluster = allocate(b'L'); entry = entries('later.txt', cluster, 1)
    data[root_at+offset:root_at+offset+len(entry)] = entry; offset += len(entry)
    assert offset + 32 <= cluster_bytes
    Path(path).write_bytes(data)
    print('Independent imported names:', len(names)+2, 'root bytes:', offset, 'cluster bytes:', cluster_bytes)
if __name__ == '__main__':
    import sys
    seed_image(sys.argv[1])
