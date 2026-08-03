#!/usr/bin/env python3
import struct, sys
import mgfs_fixture as f

B = f.B
EXT_LIST_MAGIC = 0x315458455346474D

def put64(a, off, value): struct.pack_into('<Q', a, off, value)

def set_bit(a, block):
    rel = block - 54
    a[B + 24 + rel // 8] |= 1 << (rel % 8)

def update_record(a, block, slot, record):
    off = block * B + 24 + slot * 192
    a[off:off + 192] = record
    table = bytearray(a[block * B:(block + 1) * B])
    f.checksum(table, 16, B)
    a[block * B:(block + 1) * B] = table

def mutate(path, mode):
    a = bytearray(open(path, 'rb').read())
    if mode == 'boundary':
        names = [f'f{n:024d}' for n in range(63)]
        entries = b''.join(f.entry(3 + i, n) for i, n in enumerate(names))
        entries += f.entry(66, 'shortname')
        assert len(entries) == 4080
        second = f.entry(67, 'cross')
    else:
        names = [f'f{n:024d}' for n in range(64)]
        entries = b''.join(f.entry(3 + i, n) for i, n in enumerate(names))
        assert len(entries) == B
        second = f.entry(67, names[0] if mode == 'duplicate-later' else 'later-entry')
    a[54 * B:55 * B] = entries
    if mode == 'boundary':
        a[54 * B + len(entries):55 * B] = second[:B - len(entries)]
        a[66 * B:67 * B] = second[B - len(entries):].ljust(B, b'\0')
    else:
        a[66 * B:67 * B] = second.ljust(B, b'\0')
    set_bit(a, 66)
    for rid in range(2, 68):
        rec = f.record(rid - 1, rid, 2 if rid == 2 else 1)
        update_record(a, 3 + (rid - 1) // 21, (rid - 1) % 21, rec)
    root = f.record(0, 1, 2, len(entries) + len(second), 0,
                    [(0, 54, 1, 2), (1, 66, 1, 2)])
    if mode == 'extent-list':
        list_block = 67
        set_bit(a, list_block)
        lb = bytearray(B)
        put64(lb, 0, EXT_LIST_MAGIC)
        put64(lb, 8, 1)
        put64(lb, 24, 1)
        f.extent(lb, 64, 1, 66, 1, 2)
        f.checksum(lb, 32, B)
        a[list_block * B:(list_block + 1) * B] = lb
        root = f.record(0, 1, 2, B + len(second), 0,
                        [(0, 54, 1, 2)])
        put64(root, 40, 2)
        put64(root, 56, list_block)
        f.checksum(root, 184, 192)
    elif mode == 'bad-size':
        put64(root, 32, 8193)
        f.checksum(root, 184, 192)
    elif mode == 'truncated-later':
        put64(root, 96 + 8, 16384)
        f.checksum(root, 184, 192)
    elif mode == 'bad-list':
        list_block = 67
        set_bit(a, list_block)
        lb = bytearray(B)
        put64(lb, 0, EXT_LIST_MAGIC); put64(lb, 8, 1); put64(lb, 24, 0)
        f.checksum(lb, 32, B)
        a[list_block * B:(list_block + 1) * B] = lb
        put64(root, 40, 2); put64(root, 48, 1); put64(root, 56, list_block)
        f.checksum(root, 184, 192)
    update_record(a, 3, 0, root)
    rb = bytearray(a[2 * B:3 * B])
    for rid in range(1, 68): rb[24 + (rid - 1) // 8] |= 1 << ((rid - 1) % 8)
    f.checksum(rb, 16, B)
    a[2 * B:3 * B] = rb
    ab = bytearray(a[B:2 * B])
    for block in (54, 66) + ((67,) if mode == 'extent-list' else ()):
        rel = block - 54
        ab[24 + rel // 8] |= 1 << (rel % 8)
    f.checksum(ab, 16, B)
    a[B:2 * B] = ab
    open(path, 'wb').write(a)

if __name__ == '__main__':
    mutate(sys.argv[1], sys.argv[2])
