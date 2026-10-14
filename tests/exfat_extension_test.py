#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production exFAT extension regressions with poisoned allocation slack."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[1]
compiler = next((a for a in sys.argv[1:] if not a.startswith('--')), 'gcc')
baseline = '--baseline' in sys.argv
ref = 'cb29138da7d20f1b0ea3cd96f86e6682d7752311'
def checksum(data):
    value = 0
    for index, byte in enumerate(data):
        if index not in (2, 3): value = (((value >> 1) | ((value & 1) << 15)) + byte) & 65535
    return value

def seed_imported(path, length, valid, contiguous, blocker):
    data = bytearray(path.read_bytes())
    get32 = lambda at: struct.unpack_from('<I', data, at)[0]
    heap, root_cluster, count, fat = (get32(at) for at in (88, 96, 92, 80))
    cluster_bytes = 1 << (data[108] + data[109])
    assert cluster_bytes == 4096 and data[110] == 1
    address = lambda cluster: heap * 512 + (cluster - 2) * cluster_bytes
    root_at = address(root_cluster)
    bitmap = end = None
    for offset in range(0, cluster_bytes, 32):
        at = root_at + offset
        if data[at] == 0x81: bitmap = address(get32(at + 20))
        if not data[at]: end = offset; break
    assert bitmap and end is not None and end <= 480 and (count + 7)//8 <= cluster_bytes
    free = [c for c in range(2, count + 2) if not data[bitmap + (c-2)//8] & (1 << ((c-2)%8))]
    # Poison all free data: neither imported slack nor fresh allocations can
    # obtain correct contents accidentally from a zero-filled image.
    for cluster in free: data[address(cluster):address(cluster)+cluster_bytes] = b'\x5a' * cluster_bytes
    clusters = (length + cluster_bytes - 1)//cluster_bytes
    selected = free[:clusters] if contiguous else free[:clusters*2:2]
    assert len(selected) == clusters
    if contiguous and selected: assert selected == list(range(selected[0], selected[0] + clusters))
    def mark(cluster): data[bitmap + (cluster-2)//8] |= 1 << ((cluster-2)%8)
    for i, cluster in enumerate(selected):
        mark(cluster); at = address(cluster)
        data[at:at+cluster_bytes] = b'\xa5' * cluster_bytes
        for byte in range(min(cluster_bytes, max(0, valid-i*cluster_bytes))):
            data[at+byte] = 65 + (i*cluster_bytes+byte)%23
        if not contiguous:
            struct.pack_into('<I', data, fat*512 + cluster*4,
                selected[i+1] if i+1 < clusters else 0xffffffff)
    rotate = lambda value: (value >> 1) | ((value & 1) << 15)
    def entries(name, first, data_length, valid_length, no_fat):
        entry = bytearray(96); entry[0:2] = b'\x85\x02'; entry[4] = 0x20
        timestamp = ((2000-1980)<<25) | (1<<21) | (1<<16)
        for at in (8,12,16): struct.pack_into('<I', entry, at, timestamp)
        entry[22:25] = b'\x80'*3
        entry[32] = 0xc0; entry[33] = 3 if no_fat else 1; entry[35] = len(name)
        struct.pack_into('<Q', entry, 40, valid_length); struct.pack_into('<I', entry, 52, first)
        struct.pack_into('<Q', entry, 56, data_length); entry[64] = 0xc1
        hashed = 0
        for i, char in enumerate(name):
            hashed = (rotate(hashed) + ord(char.upper())) & 65535; hashed = rotate(hashed)
            struct.pack_into('<H', entry, 66+i*2, ord(char))
        struct.pack_into('<H', entry, 36, hashed); struct.pack_into('<H', entry, 2, checksum(entry))
        return entry
    for at in range(end, 480, 32): data[root_at+at] = 0x05
    data[root_at+480:root_at+576] = entries('old.txt', selected[0] if selected else 0,
        length, valid, bool(selected) and contiguous)
    if blocker:
        blocked = selected[0] + 1; assert blocked in free and blocked not in selected
        mark(blocked); data[address(blocked):address(blocked)+cluster_bytes] = b'B'*cluster_bytes
        data[root_at+576:root_at+672] = entries('blocker', blocked, 1, 1, True)
    path.write_bytes(data)
    return root_at + 480

def verify_bytes(path, primary, length, valid, offset, size):
    data = path.read_bytes()
    get32 = lambda at: struct.unpack_from('<I', data, at)[0]
    entry = data[primary:primary+96]
    actual_length, actual_valid = (struct.unpack_from('<Q', entry, at)[0] for at in (56,40))
    final = max(length, offset+size)
    assert actual_length == final and actual_valid == max(valid, offset+size)
    assert struct.unpack_from('<H',entry,2)[0] == checksum(entry)
    cluster_bytes = 1 << (data[108] + data[109]); cluster = struct.unpack_from('<I',entry,52)[0]
    raw = bytearray(); seen = set(); heap, fat = get32(88), get32(80)
    for _ in range((final+cluster_bytes-1)//cluster_bytes):
        assert 2 <= cluster < get32(92)+2 and cluster not in seen; seen.add(cluster)
        at = heap*512 + (cluster-2)*cluster_bytes; raw.extend(data[at:at+cluster_bytes])
        cluster = cluster+1 if entry[33]&2 else get32(fat*512+cluster*4)
    expected = bytearray(final)
    for i in range(valid): expected[i] = 65+i%23
    for i in range(size): expected[offset+i] = 0x30+i%10
    assert raw[:actual_valid] == expected[:actual_valid]
    assert raw[:actual_valid] + bytes(final-actual_valid) == expected
    print('Independent initialized-prefix bytes and read-time-zero tail verified:', final, actual_valid)

with tempfile.TemporaryDirectory(prefix='mangrove-exfat-extension-') as temp:
    directory = Path(temp)
    source = root/'tests/exfat_extension_test.c'
    if baseline:
        driver = directory/'exfat.c'
        driver.write_bytes(subprocess.check_output(['git', 'show', ref+':kernel/src/storage/exfat.c'],cwd=root))
        (directory/'exfat_upcase.inc').write_bytes((root/'kernel/src/storage/exfat_upcase.inc').read_bytes())
        fixture = (root/'tests/exfat_mount_identity_test.c').read_text().replace('"../kernel/src/storage/exfat.c"','"'+str(driver)+'"').replace('"../libc/src/time_convert.c"','"'+str(root/'libc/src/time_convert.c')+'"')
        (directory/'exfat_mount_identity_test.c').write_text(fixture)
        source = directory/'extension.c'
        source.write_text((root/'tests/exfat_extension_test.c').read_text())
    binary = directory/'extension'
    subprocess.run([compiler,'-O1','-std=gnu11','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-Wno-type-limits','-ffunction-sections','-fdata-sections',
        '-fsanitize=address,undefined',*dead_strip_flags(),
        *(['-DEXFAT_EXTENSION_BASELINE'] if baseline else []),
        '-I'+str(root/'kernel/include'),'-I'+str(root/'include'),'-idirafter',str(root/'libc/include'),
        str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60)
    search = os.environ.get('PATH','') + os.pathsep + '/usr/sbin'
    mkfs, fsck = (shutil.which(name,path=search) for name in ('mkfs.exfat','fsck.exfat'))
    if not mkfs or not fsck:
        print('UNAVAILABLE: independent extension images require mkfs.exfat and fsck.exfat')
    else:
        cases = [(4,4,16,1,True,False), (4,4,4196,1,True,False)]
        if not baseline:
            cases += [(4,4,4196,1,True,True), (4,4,4196,1,False,False),
                (8000,4,4196,9,True,False), (8000,4,4196,9,False,False),
                (8000,0,512,17,False,False)]
        for index,(length,valid,offset,size,contiguous,blocker) in enumerate(cases):
            image = directory / ('extension-%u.img'%index)
            with image.open('wb') as stream: stream.truncate(12000*512)
            subprocess.run([mkfs,'-c','4096',str(image)],check=True)
            primary = seed_imported(image,length,valid,contiguous,blocker)
            subprocess.run([fsck,'-n',str(image)],check=True)
            subprocess.run([str(binary),'--image',str(image),str(offset),str(size)],check=True)
            subprocess.run([fsck,'-n',str(image)],check=True)
            if not baseline: verify_bytes(image,primary,length,valid,offset,size)
        print('Disposable images checked independently; production content readback includes a fresh mount.')
