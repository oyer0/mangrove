#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record identities versus table slots; complete production VFS/MGFS.

Only offline corruption/identity-gap encoding and host dependencies are fixtures.
The checker is independently compiled with ASan/UBSan as well as the driver.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/filesystems/mfqs'))
from runner import build, FORMAT_ARGS
from images import create
from mgfs_images import seal, U64

CC = sys.argv[1] if len(sys.argv) > 1 else 'cc'
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
folder = Path(tempfile.mkdtemp(prefix='mangrove-mgfs-checker-records-'))
try:
    adapter, tools = build(folder, CC)
    flags = [CC, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run(flags + ['-Wno-parentheses', '-Wno-unused-parameter',
        str(ROOT / 'tools/mgfsck.c'), '-o', str(tools['mgfsck'])], check=True)
    binary = folder / 'capacity'
    subprocess.run(flags + ['-Wno-unused-function',
        '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'include'),
        '-idirafter', str(ROOT / 'libc/include'),
        str(ROOT / 'tests/filesystems/mgfs/mgfs_checker_records_test.c'),
        str(ROOT / 'tests/filesystems/mfqs/host.c'),
        str(ROOT / 'kernel/src/vfs.c'), str(ROOT / 'libc/src/utf8.c'),
        '-o', str(binary)], check=True)
    image = folder / 'media.img'
    create(image, 'mgfs', 'empty', tools, FORMAT_ARGS)
    empty = image.read_bytes()
    cases = 0

    def run(command, **kwargs):
        result = subprocess.run(command, text=True, capture_output=True,
                                env=ENV, timeout=60, **kwargs)
        with (folder / 'results.log').open('a') as log:
            log.write(' '.join(map(str, command)) + '\n' + result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
        return result

    def check(data=None, error=None, warning=None):
        global cases
        if data is not None:
            image.write_bytes(data)
        before = image.read_bytes()
        result = subprocess.run([str(tools['mgfsck']), str(image)], text=True,
                                capture_output=True, env=ENV, timeout=30)
        with (folder / 'results.log').open('a') as log:
            log.write(result.stdout + result.stderr)
        assert image.read_bytes() == before, 'checker modified the image'
        assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
        if error:
            assert result.returncode == 1 and error in result.stderr, result.stdout + result.stderr
        else:
            assert result.returncode == 0 and 'Errors: 0' in result.stdout, result.stdout + result.stderr
            assert warning in result.stderr if warning else 'Warnings: 0' in result.stdout
        cases += 1

    def adapter_run(commands):
        wire = ''.join(f'MFQS1\t{i}\t{op}\t{a}\t{b}\n'
                       for i, (op, a, b) in enumerate(commands, 1))
        result = run([str(adapter), str(image)], input=wire)
        rows = [line.split('\t') for line in result.stdout.splitlines()]
        assert len(rows) == len(commands) and all(row[2] == '0' and row[6] == '0' for row in rows), result.stdout
        return rows

    def records(data):
        bitmap, table = U64(data, 120), U64(data, 136)
        result = {}
        for slot in range(U64(data, 152)):
            if data[(bitmap + slot // 32576) * 4096 + 24 + slot % 32576 // 8] & (1 << (slot % 8)):
                at = (table + slot // 21) * 4096 + 24 + slot % 21 * 192
                result[U64(data, at + 16)] = at
        return result

    def record_seal(data, at):
        seal(data, at, 192, 184)
        seal(data, at // 4096 * 4096, 4096, 16)

    check()  # Small root ID from the production formatter.
    run([str(binary), str(image), 'fill'])
    full = image.read_bytes()
    assert len(records(full)) == U64(full, 152)
    check()
    run([str(binary), str(image), 'full'])
    assert image.read_bytes() == full, 'failed full-table operations changed media'
    check()
    run([str(binary), str(image), 'reuse'])
    reused = image.read_bytes()
    assert max(records(reused)) > U64(reused, 152)
    check()
    # Remount, enumerate and open high-ID objects created through normal reuse.
    hx = lambda text: text.encode().hex()
    rows = adapter_run([('LIST', hx('/branch'), '-'),
                        ('OPEN', hx('/reuse'), '-'), ('CLOSE', '-', '-')])
    assert rows[0][7] == hx('leaf') + '00'
    check()

    # A duplicate ID hidden in an allocated, unreachable slot used to be
    # skipped entirely: the old walker enumerated IDs 1..slot_count, not slots.
    image.write_bytes(empty)
    adapter_run([('CREATE_ORDINARY', hx('/a'), '-'), ('CLOSE', '-', '-'),
                 ('CREATE_ORDINARY', hx('/b'), '-'), ('CLOSE', '-', '-')])
    data = bytearray(image.read_bytes())
    rec = records(data)
    struct.pack_into('<Q', data, rec[3] + 16, 2)
    record_seal(data, rec[3])
    root_at = U64(data, rec[1] + 72) * 4096
    second_at = root_at + ((32 + U64(data, root_at + 8) + 7) & ~7)
    struct.pack_into('<Q', data, second_at + 16, 2)
    seal(data, second_at, (32 + U64(data, second_at + 8) + 7) & ~7, 24)
    check(data, error='duplicate Record ID')

    # Imported identity gaps are legal. The following new IDs and all namespace
    # publications still come from the unmodified production allocator/driver.
    for start in (U64(empty, 152), 2**32 + 17, 2**63 + 17):
        data = bytearray(empty)
        struct.pack_into('<Q', data, 96, start)
        seal(data, 0, 200, 192)
        image.write_bytes(data)
        adapter_run([('MKDIR', hx('/dir'), '-'),
                     ('CREATE_ORDINARY', hx('/dir/file'), '-'),
                     ('WRITE', '-', b'X'.hex() * 8192), ('CLOSE', '-', '-'),
                     ('CREATE_ORDINARY', hx('/second'), '-'), ('CLOSE', '-', '-')])
        valid = image.read_bytes()
        assert set(records(valid)) == {1, start, start + 1, start + 2}
        check()
        # High-ID data still participates in complete allocation ownership.
        if start > 2**32:
            damaged = bytearray(valid)
            at = records(damaged)[start + 1]
            physical = U64(damaged, at + 72)
            bit = physical - U64(damaged, 160)
            bitmap = (U64(damaged, 104) + bit // 32576) * 4096
            damaged[bitmap + 24 + bit % 32576 // 8] &= ~(1 << (bit % 8))
            seal(damaged, bitmap, 4096, 16)
            check(damaged, error='is marked free')

    # Highest identity the production allocator can assign (UINT64_MAX is the
    # exhausted next-ID sentinel); no array is sized using this identity.
    data = bytearray(empty)
    struct.pack_into('<Q', data, 96, 2**64 - 2)
    seal(data, 0, 200, 192)
    image.write_bytes(data)
    adapter_run([('CREATE_ORDINARY', hx('/last'), '-'), ('CLOSE', '-', '-')])
    check()

    # Negative controls retain valid checksums. Distinguish an ID with no
    # allocated slot from a large, valid ID, and reject duplicate identities.
    rec = records(valid)
    directory, file, second = rec[start], rec[start + 1], rec[start + 2]
    root = rec[1]
    directory_at = U64(valid, directory + 72) * 4096
    for missing in (0, U64(valid, 152) - 1, 2**64 - 1):
        data = bytearray(valid)
        struct.pack_into('<Q', data, directory_at, missing)
        length = (32 + U64(data, directory_at + 8) + 7) & ~7
        seal(data, directory_at, length, 24)
        check(data, error='references free Record')
    data = bytearray(valid)
    struct.pack_into('<Q', data, second + 16, start + 1)
    record_seal(data, second)
    check(data, error='duplicate Record ID')
    data = bytearray(valid)
    struct.pack_into('<Q', data, file + 16, 0)
    record_seal(data, file)
    check(data, error='allocated but malformed')
    data = bytearray(valid)
    struct.pack_into('<Q', data, file + 16, U64(data, 96))
    record_seal(data, file)
    check(data, error='allocated but malformed')
    data = bytearray(valid)
    data[file + 184] ^= 1
    check(data, error='allocated but malformed')

    # The only live entry in the high-ID directory becomes a self-reference.
    data = bytearray(valid)
    struct.pack_into('<Q', data, directory_at, start)
    seal(data, directory_at, (32 + U64(data, directory_at + 8) + 7) & ~7, 24)
    check(data, error='directory cycle')
    # A second entry to the same high-ID file must still be diagnosed.
    data = bytearray(valid)
    root_at = U64(data, root + 72) * 4096
    first_length = (32 + U64(data, root_at + 8) + 7) & ~7
    second_at = root_at + first_length
    struct.pack_into('<Q', data, second_at, start + 1)
    seal(data, second_at, (32 + U64(data, second_at + 8) + 7) & ~7, 24)
    check(data, error='referenced by multiple live entries')
    # Remove only the live edge, keeping its high-ID allocation/extent intact.
    data = bytearray(valid)
    struct.pack_into('<Q', data, directory_at + 16, 2)
    seal(data, directory_at, (32 + U64(data, directory_at + 8) + 7) & ~7, 24)
    check(data, warning=f'Record {start + 1} is allocated but unreachable')
    print(f'MGFS checker Records: {cases} checks passed; production capacity, '
          'unchanged full-table failures, slot reuse, 64-bit IDs, ownership, '
          'enumeration and corruption controls; ASan/UBSan; LSan disabled')
except BaseException:
    print('Failure artifacts preserved:', folder, file=sys.stderr)
    raise
else:
    shutil.rmtree(folder)
