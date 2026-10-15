#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production exFAT zero-allocation lifecycle and VFS dispatch."""
from pathlib import Path
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags, empty_namespace_dependencies

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/vfs.c').read_text()

def function(name):
    for match in re.finditer(r'^(?:static )?[^\n;{}]*\b' + name + r'\s*\(', source, re.M):
        if source[match.start():match.end()].lstrip().startswith(('if ', 'if(', 'return ')):
            continue
        brace = source.find('{', match.end())
        if ';' in source[match.end():brace]: continue
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}'); end += 1
        return source[match.start():end] + '\n'
    raise AssertionError(name)

dependencies = empty_namespace_dependencies(source) + r'''
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
static vfs_mount_t *vfs_find_mount_child_locked(vfs_node_t *dir, const char *name) { (void)dir; (void)name; return NULL; }
static vfs_node_t *vfs_finddir_locked(vfs_node_t *dir, const char *name) { return dir->ops->finddir(dir,name); }
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
names = (
    'vfs_node_on_read_only_mount', 'vfs_node_retain_super',
    'vfs_super_operation_lock', 'vfs_super_operation_unlock', 'vfs_node_operation_begin', 'vfs_node_operation_end',
    'vfs_node_register_created', 'vfs_node_unregister_created', 'vfs_operation_finish_pin', 'vfs_super_operation_complete', 'vfs_node_operation_finish',
    'vfs_create_node', 'vfs_create', 'vfs_mkdir',
    'vfs_unlink_locked', 'vfs_unlink', 'vfs_rmdir_locked', 'vfs_rmdir',
)
compiler = sys.argv[1] if len(sys.argv) > 1 else 'gcc'

def seed_zero_directories(path):
    """Independent encoder: add two unallocated directories and a later file."""
    data = bytearray(path.read_bytes())
    heap, first = struct.unpack_from('<I', data, 88)[0], struct.unpack_from('<I', data, 96)[0]
    cluster_bytes = 1 << (data[108] + data[109])
    root_at = heap*512 + (first-2)*cluster_bytes
    end = 0
    while data[root_at+end]: end += 32
    assert end <= 480 and cluster_bytes >= 1024
    for at in range(end, 480, 32): data[root_at+at] = 0x05
    rotate = lambda n: (n >> 1) | ((n & 1) << 15)
    at = 480
    for name, directory in [('empty', True), ('other', True), ('later.txt', False)]:
        entries = bytearray(96)
        entries[0] = 0x85; entries[1] = 2
        struct.pack_into('<H', entries, 4, 0x10 if directory else 0x20)
        # Deterministic ordinary fixture timestamps: 2000-01-01 midnight UTC.
        timestamp = ((2000 - 1980) << 25) | (1 << 21) | (1 << 16)
        for offset in (8, 12, 16): struct.pack_into('<I', entries, offset, timestamp)
        entries[22:25] = b'\x80\x80\x80'  # Valid zero UTC offsets.
        entries[32] = 0xc0; entries[33] = 1; entries[35] = len(name)
        entries[64] = 0xc1
        hashed = 0
        for i, char in enumerate(name):
            unit = ord(char.upper())
            hashed = (rotate(hashed) + (unit & 255)) & 65535
            hashed = (rotate(hashed) + (unit >> 8)) & 65535
            struct.pack_into('<H', entries, 66+2*i, ord(char))
        struct.pack_into('<H', entries, 36, hashed)
        checksum = 0
        for i, byte in enumerate(entries):
            if i not in (2,3): checksum = (rotate(checksum)+byte) & 65535
        struct.pack_into('<H', entries, 2, checksum)
        data[root_at+at:root_at+at+96] = entries; at += 96
    path.write_bytes(data)

with tempfile.TemporaryDirectory(prefix='mangrove-exfat-zero-') as directory:
    path = Path(directory)
    (path / 'vfs_zero_directory.inc').write_text(dependencies + ''.join(function(n) for n in names))
    binary = path / 'zero'
    subprocess.run([compiler, '-O1', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-type-limits', '-ffunction-sections', '-fdata-sections',
        '-fsanitize=address,undefined', *dead_strip_flags(), '-I'+str(path),
        '-I'+str(root/'kernel/include'), '-I'+str(root/'include'),
        '-idirafter', str(root/'libc/include'), str(root/'tests/exfat_zero_directory_test.c'),
        '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    search = os.environ.get('PATH', '') + os.pathsep + '/usr/sbin'
    mkfs, fsck = (shutil.which(n, path=search) for n in ('mkfs.exfat', 'fsck.exfat'))
    if not mkfs or not fsck:
        print('SKIP independent zero-directory images: install exfatprogs.')
    else:
        image = path/'zero.img'
        with image.open('wb') as stream: stream.truncate(12000*512)
        subprocess.run([mkfs, '-c', '4096', str(image)], check=True)
        seed_zero_directories(image)
        subprocess.run([fsck, '-n', str(image)], check=True)
        subprocess.run([str(binary), '--populate', str(image)], check=True)
        subprocess.run([fsck, '-n', str(image)], check=True)
        subprocess.run([str(binary), '--remove', str(image)], check=True)
        subprocess.run([fsck, '-n', str(image)], check=True)
        print('Independent mkfs.exfat zero directories, VFS mutations, fresh-state readback and fsck.exfat passed.')
