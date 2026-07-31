#!/usr/bin/env python3
import struct, sys
import mgfs_fixture as f

B = f.B

def put64(a, off, value): struct.pack_into('<Q', a, off, value)
def set_bit(a, block):
    rel = block - 54
    a[B + 24 + rel // 8] |= 1 << (rel % 8)
def update_record(a, block, slot, record):
    off = block * B + 24 + slot * 192
    a[off:off + 192] = record
    table = bytearray(a[block * B:(block + 1) * B]); f.checksum(table, 16, B)
    a[block * B:(block + 1) * B] = table
def set_record(a, rid, typ, block):
    update_record(a, 3 + (rid - 1) // 21, (rid - 1) % 21,
                  f.record(rid - 1, rid, typ, 0 if typ == 2 else 0,
                           0, [] if typ == 2 else ()))
    if typ == 2 and block is not None:
        update_record(a, 3 + (rid - 1) // 21, (rid - 1) % 21,
                      f.record(rid - 1, rid, typ, 0, 0, [(0, block, 1, 2)]))
        set_bit(a, block)
def write_dir(a, block, entries):
    data = b''.join(f.entry(rid, name) for rid, name in entries)
    a[block * B:(block + 1) * B] = data.ljust(B, b'\0')
    set_bit(a, block)
    return len(data)
def mutate(path, mode):
    a = bytearray(open(path, 'rb').read())
    for rid in (3, 4, 5): set_record(a, rid, 2, 66 + rid - 3)
    set_record(a, 6, 1, None)
    root_entries = []
    child = {}
    if mode == 'self': child[3] = [(3, 'self')]; root_entries = [(3, 'A')]
    elif mode == 'two-cycle': child[3] = [(4, 'B')]; child[4] = [(3, 'A')]; root_entries = [(3, 'A')]
    elif mode == 'long-cycle': child[3] = [(4, 'B')]; child[4] = [(5, 'C')]; child[5] = [(3, 'A')]; root_entries = [(3, 'A')]
    elif mode == 'root-cycle': child[3] = [(1, 'root')]; root_entries = [(3, 'A')]
    elif mode == 'multi-parent': child[3] = [(5, 'C')]; child[4] = [(5, 'D')]; root_entries = [(3, 'A'), (4, 'B')]
    elif mode == 'same-parent': child[3] = [(5, 'C'), (5, 'D')]; root_entries = [(3, 'A')]
    elif mode == 'regular-twice': root_entries = [(6, 'one'), (6, 'two')]
    elif mode == 'deep': child[3] = [(4, 'B')]; child[4] = [(5, 'C')]; root_entries = [(3, 'A')]
    elif mode == 'unreachable-cycle':
        set_record(a, 7, 2, 69); set_record(a, 8, 2, 70)
        child[7] = [(8, 'B')]; child[8] = [(7, 'A')]; root_entries = []
    else: raise SystemExit('unknown graph fixture')
    for rid, entries in child.items():
        block = 66 + rid - 3 if rid < 7 else 69 + rid - 7
        size = write_dir(a, block, entries)
        update_record(a, 3 + (rid - 1) // 21, (rid - 1) % 21,
                      f.record(rid - 1, rid, 2, size, 0, [(0, block, 1, 2)]))
    root_data = b''.join(f.entry(rid, name) for rid, name in root_entries)
    a[54 * B:55 * B] = root_data.ljust(B, b'\0'); set_bit(a, 54)
    root = f.record(0, 1, 2, len(root_data), 0, [(0, 54, 1, 2)])
    update_record(a, 3, 0, root)
    rb = bytearray(a[2 * B:3 * B])
    for rid in range(1, 7): rb[24 + (rid - 1) // 8] |= 1 << ((rid - 1) % 8)
    f.checksum(rb, 16, B); a[2 * B:3 * B] = rb
    ab = bytearray(a[B:2 * B]); f.checksum(ab, 16, B); a[B:2 * B] = ab
    open(path, 'wb').write(a)
if __name__ == '__main__': mutate(sys.argv[1], sys.argv[2])
