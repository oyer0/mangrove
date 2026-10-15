# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline MGFS directory fixtures. No production I/O or namespace emulation."""
import struct

BLOCK = 4096
U64 = lambda data, at: struct.unpack_from('<Q', data, at)[0]


def seal(data, at, length, checksum):
    value = 0
    for index, byte in enumerate(data[at:at + length]):
        value ^= (0 if checksum <= index < checksum + 8 else byte) << 56
        for _ in range(8):
            value = ((value << 1) ^ (0x42f0e1eba9ea3693 if value >> 63 else 0)) & ((1 << 64) - 1)
    struct.pack_into('<Q', data, at + checksum, value)


def entry(identifier, name, flags=1):
    name = name.encode('ascii')
    result = bytearray((32 + len(name) + 7) & ~7)
    struct.pack_into('<QQQ', result, 0, identifier, len(name), flags)
    result[32:32 + len(name)] = name
    seal(result, 0, len(result), 24)
    return result


def preallocate(data, identifier=1, runs=(1, 1, 1, 1), logical_size=None, inline_count=2):
    """Replace a directory's mapping, preserving its logical entry stream.

    Choose unique free blocks, with a free block between runs. Padding consists
    of valid tombstones. Unused capacity is poisoned, including a checksum-valid
    live root backreference that must never be interpreted below a false EOF.
    The independent checker validates the resulting image before runtime use.
    """
    table, table_blocks = U64(data, 136), U64(data, 144)
    records = [table * BLOCK + block * BLOCK + 24 + slot * 192
               for block in range(table_blocks) for slot in range(21)]
    record = next(at for at in records if U64(data, at) == 2 and U64(data, at + 16) == identifier)
    bitmap, start, count = U64(data, 104), U64(data, 160), U64(data, 168)
    touched = set()

    def allocated(block, state=None):
        bit = block - start
        at = (bitmap + bit // 32576) * BLOCK + 24 + bit % 32576 // 8
        mask = 1 << (bit % 8)
        if state is not None:
            data[at] = data[at] | mask if state else data[at] & ~mask
            touched.add(at // BLOCK)
        return bool(data[at] & mask)

    extents = [struct.unpack_from('<QQQQ', data, record + 64 + i * 32)
               for i in range(U64(data, record + 48))]
    old_lists, block = [], U64(data, record + 56)
    while block:
        old_lists.append(block)
        extents += [struct.unpack_from('<QQQQ', data, block * BLOCK + 64 + i * 32)
                    for i in range(U64(data, block * BLOCK + 24))]
        block = U64(data, block * BLOCK + 16)
    old_size = U64(data, record + 32)
    contents = b''.join(data[physical * BLOCK:(physical + blocks) * BLOCK]
                        for _, physical, blocks, _ in extents)[:old_size]
    assert len(contents) == old_size
    for _, physical, blocks, _ in extents:
        for block in range(physical, physical + blocks): allocated(block, False)
    for block in old_lists: allocated(block, False)
    if logical_size is None: logical_size = old_size
    assert logical_size >= old_size and logical_size <= sum(runs) * BLOCK
    padding = logical_size - old_size
    while padding:
        length = min(288, padding)
        if 0 < padding - length < 40: length -= 40 - (padding - length)
        assert length >= 40 and length % 8 == 0
        contents += entry(1, 't' * min(255, length - 32), 2)
        padding -= length
    assert len(contents) == logical_size
    poison = bytes(entry(1, 'ghost')) + b'\xa5' * (BLOCK - 40)
    stream = bytearray(poison * sum(runs))
    stream[:logical_size] = contents
    if logical_size + 40 <= len(stream): stream[logical_size:logical_size + 40] = entry(1, 'ghost')
    selected, logical, candidate = [], 0, start
    for blocks in runs:
        while candidate + blocks <= start + count and any(allocated(b) for b in range(candidate, candidate + blocks)):
            candidate += 1
        assert blocks and candidate + blocks <= start + count
        selected.append((logical, candidate, blocks, 2))
        data[candidate * BLOCK:(candidate + blocks) * BLOCK] = stream[logical * BLOCK:(logical + blocks) * BLOCK]
        for block in range(candidate, candidate + blocks): allocated(block, True)
        logical += blocks
        candidate += blocks + 1
    inline_count = min(inline_count, len(selected))
    assert len(selected) > 2 or inline_count == len(selected)
    free = (block for block in range(start, start + count) if not allocated(block))
    lists = [next(free) for _ in range((max(0, len(selected) - inline_count) + 125) // 126)]
    for index, block in enumerate(lists):
        allocated(block, True)
        at = block * BLOCK
        values = selected[inline_count + index * 126:inline_count + (index + 1) * 126]
        data[at:at + BLOCK] = bytes(BLOCK)
        struct.pack_into('<QQQQ', data, at, 0x315458455346474D, identifier,
                         lists[index + 1] if index + 1 < len(lists) else 0, len(values))
        for i, extent in enumerate(values): struct.pack_into('<QQQQ', data, at + 64 + i * 32, *extent)
        seal(data, at, BLOCK, 32)
    struct.pack_into('<QQQQ', data, record + 32, logical_size, len(selected), inline_count, lists[0] if lists else 0)
    data[record + 64:record + 128] = bytes(64)
    for i, extent in enumerate(selected[:inline_count]): struct.pack_into('<QQQQ', data, record + 64 + i * 32, *extent)
    seal(data, record, 192, 184)
    seal(data, record // BLOCK * BLOCK, BLOCK, 16)
    for block in touched: seal(data, block * BLOCK, BLOCK, 16)
    return dict(directory_record=identifier, logical_bytes=logical_size,
                allocated_blocks=sum(runs), extents=len(runs), poison_byte=0xa5)
