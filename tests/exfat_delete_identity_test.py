#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production exFAT unique deletion, actual VFS routes and disposable images."""
from pathlib import Path
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[1]
compiler = next((a for a in sys.argv[1:] if not a.startswith('--')), 'gcc')
baseline = '--baseline' in sys.argv
ref = '4f75dccbf2ebcf5712cace210dfa904c3478297b'
vfs = (root/'kernel/src/vfs.c').read_text()

def function(name):
    for match in re.finditer(r'^(?:static )?[^\n;{}]*\b'+name+r'\s*\(', vfs, re.M):
        if vfs[match.start():match.end()].lstrip().startswith(('if ', 'if(', 'return ')): continue
        brace = vfs.find('{', match.end())
        if ';' in vfs[match.end():brace]: continue
        depth, end = 1, brace+1
        while depth:
            depth += (vfs[end] == '{') - (vfs[end] == '}'); end += 1
        return vfs[match.start():end]+'\n'
    raise AssertionError(name)

dependencies = r'''
static spinlock_t vfs_metadata_lock;
static bool operation_held;
static u64 metadata_enter(spinlock_t *lock) { assert(lock == &vfs_metadata_lock); return 0; }
static void metadata_leave(spinlock_t *lock, u64 flags) { assert(lock == &vfs_metadata_lock); (void)flags; }
#define spin_lock_irqsave(lock) metadata_enter(lock)
#define spin_unlock_irqrestore(lock, flags) metadata_leave(lock, flags)
bool mutex_lock(mutex_t *lock) { assert(lock == &super.operation_lock && !operation_held); operation_held=true; return true; }
bool mutex_unlock(mutex_t *lock) { assert(lock == &super.operation_lock && operation_held); operation_held=false; return true; }
static bool vfs_super_is_live_locked(const vfs_super_t *sb) { return sb == &super && online && sb->state != VFS_SUPER_DEAD; }
bool vfs_super_is_live(const vfs_super_t *sb) { return vfs_super_is_live_locked(sb); }
bool vfs_node_is_live(const vfs_node_t *node) { return node && vfs_super_is_live(node->super); }
void vfs_super_release(vfs_super_t *sb) { assert(sb == &super && sb->reference_count > 1U); sb->reference_count--; }
static vfs_node_t *vfs_finddir_locked(vfs_node_t *dir, const char *name) { return dir->ops->finddir(dir, name); }
bool vfs_directory_child_mutation_allowed(const vfs_node_t *parent, const vfs_node_t *child) { (void)parent; (void)child; return true; }
int vfs_recovery_pin_finish_handoff(u64 pin) { assert(pin && !operation_held && pin_active && !pin_unresolved); if (!pin_handoff(&super, pin_context)) { assert(vfs_recovery_pin_mark_unresolved(pin) == VFS_OK); return VFS_ERR_UNCERTAIN; } pin_active=false; pin_handoff=NULL; pin_context=NULL; return VFS_OK; }
/* exFAT does not opt into the MGFS guard. Assert that boundary while using
 * actual VFS operation dispatch/completion below. */
static void *thread_current(void){return (void *)1;}
bool vfs_super_retain(vfs_super_t *sb){(void)sb;assert(false);return false;}
int vfs_super_operation_error(const vfs_super_t *sb){return vfs_super_is_live(sb)?VFS_OK:VFS_ERR_DEVICE_GONE;}
void vfs_super_operation_pause(vfs_super_t *sb){(void)mutex_unlock(&sb->operation_lock);}
bool vfs_super_operation_guard_release(vfs_super_t *sb){assert(!sb->serialize_handoff);return true;}

'''
functions = (
    'vfs_node_on_read_only_mount', 'vfs_node_retain_super',
    'vfs_super_operation_lock', 'vfs_super_operation_unlock', 'vfs_node_operation_begin', 'vfs_node_operation_end',
    'vfs_operation_finish_pin', 'vfs_super_operation_complete', 'vfs_node_operation_finish', 'vfs_unlink_locked', 'vfs_unlink', 'vfs_unlink_trusted',
    'vfs_rmdir_locked', 'vfs_rmdir', 'vfs_rmdir_trusted',
)

def seed_names(path, directory, duplicate, unicode):
    """Independent encoder using the formatter's own compressed upcase table."""
    data = bytearray(path.read_bytes())
    get32 = lambda at: struct.unpack_from('<I', data, at)[0]
    heap, first, count = (get32(at) for at in (88, 96, 92))
    cb = 1 << (data[108]+data[109]); assert cb == 4096
    address = lambda c: heap*512 + (c-2)*cb
    root_at = address(first)
    bitmap = table = end = None
    for offset in range(0, cb, 32):
        at = root_at+offset
        if data[at] == 0x81: bitmap = address(get32(at+20))
        if data[at] == 0x82:
            raw = data[address(get32(at+20)):][:struct.unpack_from('<Q', data, at+24)[0]]
            table = []; index = 0
            while index < len(raw):
                value = struct.unpack_from('<H', raw, index)[0]; index += 2
                if value == 65535 and len(table) != 65535:
                    run = struct.unpack_from('<H', raw, index)[0]; index += 2
                    table.extend(range(len(table), len(table)+run))
                else: table.append(value)
        if not data[at]: end = offset; break
    assert bitmap and table and len(table) == 65536 and end <= 480
    free = [c for c in range(2, count+2) if not data[bitmap+(c-2)//8] & (1 << ((c-2)%8))]
    rotate = lambda n: (n >> 1) | ((n & 1) << 15)
    names = ['café.txt', 'CAFÉ.TXT' if duplicate else 'unrelated'] if unicode else ['same.txt', 'SAME.TXT' if duplicate else 'unrelated']
    for at in range(end, 1504, 32): data[root_at+at] = 0x05
    for offset, name, cluster in zip((480, 1504), names, free):
        entry = bytearray(96); entry[0:2] = b'\x85\x02'; entry[4] = 0x10 if directory else 0x20
        for at in (8, 12, 16): struct.pack_into('<I', entry, at, (20<<25)|(1<<21)|(1<<16))
        entry[22:25] = b'\x80'*3; entry[32] = 0xc0; entry[33] = 3; entry[35] = len(name)
        length = cb if directory else 1
        struct.pack_into('<Q', entry, 40, length); struct.pack_into('<I', entry, 52, cluster)
        struct.pack_into('<Q', entry, 56, length); entry[64] = 0xc1
        hashed = 0
        for i, char in enumerate(name):
            unit = ord(char); upper = table[unit]
            hashed = (rotate(hashed)+(upper & 255)) & 65535
            hashed = (rotate(hashed)+(upper >> 8)) & 65535
            struct.pack_into('<H', entry, 66+2*i, unit)
        struct.pack_into('<H', entry, 36, hashed)
        checksum = 0
        for i, byte in enumerate(entry):
            if i not in (2, 3): checksum = (rotate(checksum)+byte) & 65535
        struct.pack_into('<H', entry, 2, checksum)
        data[root_at+offset:root_at+offset+96] = entry
        data[bitmap+(cluster-2)//8] |= 1 << ((cluster-2)%8)
        data[address(cluster):address(cluster)+cb] = bytes(cb) if directory else b'D'+bytes(cb-1)
    path.write_bytes(data)
    return 'CAFÉ.TXT' if unicode else 'SAME.TXT'

with tempfile.TemporaryDirectory(prefix='mangrove-exfat-delete-') as temp:
    directory = Path(temp)
    for name in ('exfat_delete_identity_test.c', 'exfat_names_test.c', 'exfat_mount_identity_test.c'):
        text = (root/'tests'/name).read_text()
        if name == 'exfat_names_test.c': text = text.split('\nint main(', 1)[0]
        if name == 'exfat_mount_identity_test.c':
            driver = root/'kernel/src/storage/exfat.c'
            if baseline:
                driver = directory/'exfat.c'
                driver.write_bytes(subprocess.check_output(['git','show',ref+':kernel/src/storage/exfat.c'],cwd=root))
                (directory/'exfat_upcase.inc').write_bytes((root/'kernel/src/storage/exfat_upcase.inc').read_bytes())
            text = text.replace('"../kernel/src/storage/exfat.c"', '"'+str(driver)+'"')
            text = text.replace('"../libc/src/time_convert.c"', '"'+str(root/'libc/src/time_convert.c')+'"')
        (directory/name).write_text(text)
    (directory/'delete_vfs.inc').write_text(dependencies+''.join(function(n) for n in functions))
    binary = directory/'delete'
    command = [compiler,'-O1','-std=gnu11','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-Wno-type-limits','-ffunction-sections','-fdata-sections',
        '-fsanitize=address,undefined',*dead_strip_flags(),
        '-I'+str(root/'kernel/include'),'-I'+str(root/'include'),'-idirafter',str(root/'libc/include'),
        str(directory/'exfat_delete_identity_test.c'),'-o',str(binary)]
    subprocess.run(command,check=True)
    if baseline:
        result = subprocess.run([str(binary)],capture_output=True,text=True,timeout=60)
        assert result.returncode != 0 and 'result == expected' in result.stderr, result.stderr
        print('Corrected zero-write rejection assertion FAILS against committed baseline as expected.')
        subprocess.run(command+['-DEXFAT_DELETE_BASELINE'],check=True)
    subprocess.run([str(binary)],check=True,timeout=60)
    search = os.environ.get('PATH','')+os.pathsep+'/usr/sbin'
    mkfs, fsck = (shutil.which(name,path=search) for name in ('mkfs.exfat','fsck.exfat'))
    if not mkfs or not fsck:
        print('UNAVAILABLE: independent deletion images require mkfs.exfat and fsck.exfat')
    else:
        cases = [(d, True, False) for d in (False, True)]
        if not baseline: cases += [(d, duplicate, True) for d in (False, True) for duplicate in (False, True)]
        for i, (is_dir, duplicate, unicode) in enumerate(cases):
            image = directory/('delete-%u.img'%i)
            with image.open('wb') as f: f.truncate(12000*512)
            subprocess.run([mkfs,'-c','4096',str(image)],check=True)
            name = seed_names(image,is_dir,duplicate,unicode)
            subprocess.run([fsck,'-n',str(image)],check=True)
            before = hashlib.sha256(image.read_bytes()).digest()
            subprocess.run([str(binary),'--image',str(image),name,str(int(is_dir)),str(int(duplicate))],check=True)
            subprocess.run([fsck,'-n',str(image)],check=True)
            if duplicate and not baseline: assert hashlib.sha256(image.read_bytes()).digest() == before
        print('Independent disposable baseline deletions reproduced.' if baseline else
              'Independent disposable deletion images passed; rejected images byte-identical.')
