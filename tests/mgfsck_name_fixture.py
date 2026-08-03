#!/usr/bin/env python3
import struct, sys

POLY = 0x42F0E1EBA9EA3693
B = 4096

def crc(data):
    c = 0
    for byte in data:
        c ^= byte << 56
        for _ in range(8):
            c = ((c << 1) ^ POLY) & ((1 << 64) - 1) if c & (1 << 63) else (c << 1) & ((1 << 64) - 1)
    return c

def w64(buf, off, value):
    struct.pack_into('<Q', buf, off, value)

def mutate(path, mode):
    data = bytearray(open(path, 'rb').read())
    root_block = 54 * B
    first = root_block
    second = root_block + 48
    if mode == 'duplicate':
        data[second + 32:second + 48] = b'\0' * 16
        data[second + 32:second + 41] = b'empty.txt'
        w64(data, second + 8, 9)
        w64(data, second + 24, 0)
        w64(data, second + 24, crc(data[second:second + 48]))
    elif mode == 'live-tombstone' or mode == 'tombstone-tombstone':
        w64(data, second + 16, 2)
        w64(data, second + 24, 0)
        w64(data, second + 24, crc(data[second:second + 48]))
        if mode == 'tombstone-tombstone':
            w64(data, first + 16, 2)
            w64(data, first + 24, 0)
            w64(data, first + 24, crc(data[first:first + 48]))
    elif mode == 'bad-length':
        w64(data, second + 8, 250)
        w64(data, second + 24, 0)
        w64(data, second + 24, crc(data[second:second + 48]))
    elif mode == 'truncated':
        w64(data, 3 * B + 24 + 32, 60)
        w64(data, 3 * B + 24 + 184, 0)
        record_off = 3 * B + 24
        record = bytearray(data[record_off:record_off + 192])
        w64(record, 184, 0)
        w64(data, record_off + 184, crc(record))
    else:
        raise SystemExit('unknown fixture mode')
    open(path, 'wb').write(data)

if __name__ == '__main__':
    mutate(sys.argv[1], sys.argv[2])
