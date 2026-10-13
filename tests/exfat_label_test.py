#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production exFAT label transaction and private VFS owner, disposable media."""
from pathlib import Path
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[1]
source = (root/'kernel/src/vfs.c').read_text()
def function(name):
    for m in re.finditer(r'^(?:static )?[^\n;{}]*\b'+name+r'\s*\(', source, re.M):
        brace = source.find('{', m.end())
        if ';' in source[m.end():brace]: continue
        depth, end = 1, brace+1
        while depth:
            depth += (source[end]=='{') - (source[end]=='}'); end += 1
        return source[m.start():end]+'\n'
    raise AssertionError(name)

def fill_directory(path, no_marker):
    """Independent checksummed empty file sets consume the first root cluster."""
    data = bytearray(path.read_bytes())
    heap, root_cluster = struct.unpack_from('<I', data, 88)[0], struct.unpack_from('<I', data, 96)[0]
    cluster_bytes = 1 << (data[108]+data[109])
    assert cluster_bytes == 4096
    at = heap*512 + (root_cluster-2)*cluster_bytes
    system = [bytes(data[pos:pos+32]) for pos in range(at, at+cluster_bytes, 32)
              if data[pos] in (0x81,0x82)]
    assert len(system) == 2
    directory = bytearray(cluster_bytes)
    directory[:64] = b''.join(system)
    entries = [('f%02u'%i) for i in range(42 if no_marker else 40)]
    if not no_marker: entries.append('L'*31) # five slots; END is the last slot
    rotate = lambda n: (n>>1)|((n&1)<<15)
    pos = 64
    for name in entries:
        count = 2+(len(name)+14)//15
        entry = bytearray(count*32)
        entry[0]=0x85; entry[1]=count-1
        # Independent valid UTC epoch dates; no driver encoding helper.
        for off in (8,12,16): struct.pack_into('<I', entry, off, (20<<25)|(1<<21)|(1<<16))
        entry[22:25]=b'\x80'*3
        entry[32]=0xc0; entry[33]=1; entry[35]=len(name)
        hashed=0
        for i, char in enumerate(name):
            upper=ord(char.upper())
            hashed=(rotate(hashed)+(upper&255))&65535
            hashed=(rotate(hashed)+(upper>>8))&65535
            page, unit=divmod(i,15)
            entry[64+page*32]=0xc1
            struct.pack_into('<H',entry,66+page*32+unit*2,ord(char))
        struct.pack_into('<H',entry,36,hashed)
        checksum=0
        for i, byte in enumerate(entry):
            if i not in (2,3): checksum=(rotate(checksum)+byte)&65535
        struct.pack_into('<H',entry,2,checksum)
        directory[pos:pos+len(entry)]=entry; pos+=len(entry)
    assert pos == cluster_bytes-(0 if no_marker else 32)
    data[at:at+cluster_bytes]=directory
    path.write_bytes(data)

names = ('vfs_device_range', 'vfs_device_already_mounted',
    'vfs_retarget_nodes_locked', 'vfs_super_is_live_locked',
    'vfs_super_mark_dispose_locked', 'vfs_super_destroy', 'vfs_super_release',
    'vfs_recovery_private_begin', 'vfs_recovery_private_release',
    'vfs_recovery_pin_mark_unresolved', 'vfs_recovery_pin_finish_handoff',
    'vfs_recovery_pins_snapshot', 'vfs_format_preflight')
registry = source[source.index('typedef struct vfs_recovery_pin {'):source.index('static vfs_mount_t *vfs_find_mount_child')]
registry += '\nstatic vfs_mount_t mount_table[VFS_MAX_MOUNTS];\nstatic vfs_super_t dead_super;\nstatic spinlock_t vfs_metadata_lock;\nstatic mutex_t vfs_mount_lock;\n'
compiler = sys.argv[1] if len(sys.argv)>1 else 'gcc'
search = os.environ.get('PATH','') + os.pathsep + '/usr/sbin'
mkfs, fsck = (shutil.which(n, path=search) for n in ('mkfs.exfat','fsck.exfat'))
if not mkfs or not fsck:
    raise SystemExit('exfatprogs required: install mkfs.exfat and fsck.exfat for label verification')
with tempfile.TemporaryDirectory(prefix='mangrove-exfat-label-') as temp:
    directory=Path(temp)
    (directory/'vfs_label_owner.inc').write_text(registry + '\n'.join(function(n) for n in names))
    binary=directory/'label'
    subprocess.run([compiler,'-O1','-std=gnu11','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-Wno-type-limits','-ffunction-sections','-fdata-sections',
        '-fsanitize=address,undefined',*dead_strip_flags(),'-I'+str(directory),
        '-I'+str(root/'kernel/include'),'-I'+str(root/'include'),'-idirafter',str(root/'libc/include'),
        str(root/'tests/exfat_label_test.c'),'-o',str(binary)],check=True)
    for layout in ('existing','end-marker','full'):
        image=directory/(layout+'.img')
        with image.open('wb') as stream: stream.truncate(12000*512)
        subprocess.run([mkfs,'-c','4096','-L','original',str(image)],check=True)
        if layout != 'existing': fill_directory(image, layout=='full')
        subprocess.run([fsck,'-n',str(image)],check=True)
        subprocess.run([str(binary),str(image),layout],check=True)
        subprocess.run([fsck,'-n',str(image)],check=True)
    print('Independent formatted images: label replacement, marker relocation and full-root growth verified.')
