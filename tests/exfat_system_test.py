#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production exFAT/VFS visibility and independently encoded disposable media."""
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
source = (root / 'kernel/src/vfs.c').read_text()
def function(name):
    for match in re.finditer(r'^(?:static )?[^\n;{}]*\b'+name+r'\s*\(', source, re.M):
        if source[match.start():match.end()].lstrip().startswith(('if ', 'if(', 'return ')): continue
        brace = source.find('{', match.end())
        if ';' in source[match.end():brace]: continue
        depth, end = 1, brace+1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}'); end += 1
        return source[match.start():end]+'\n'
    raise AssertionError(name)

dependencies = r'''
/* Controlled namespace admission: this visibility fixture has no locations. */
#define NAMESPACE_SCOPE(failure) ((void)0)
static void location_note_child(vfs_node_t *p,const char *n,vfs_node_t *c){(void)p;(void)n;(void)c;}
static void location_detach(vfs_node_t *n){(void)n;}

#define VFS_FILE_HANDLE_VALID 0x56465348U
static spinlock_t vfs_metadata_lock;
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static bool operation_held;
static mutex_t *offset_held;
static u64 metadata_enter(spinlock_t *lock) { assert(lock == &vfs_metadata_lock); return 0; }
static void metadata_leave(spinlock_t *lock, u64 flags) { assert(lock == &vfs_metadata_lock); (void)flags; }
#define spin_lock_irqsave(lock) metadata_enter(lock)
#define spin_unlock_irqrestore(lock, flags) metadata_leave(lock, flags)
void mutex_init(mutex_t *lock) { memset(lock, 0, sizeof(*lock)); }
bool mutex_lock(mutex_t *lock) {
    if (lock == &super.operation_lock) { assert(!operation_held); operation_held=true; }
    else { assert(!offset_held); offset_held=lock; }
    return true;
}
bool mutex_unlock(mutex_t *lock) {
    if (lock == &super.operation_lock) { assert(operation_held); operation_held=false; }
    else { assert(offset_held == lock); offset_held=NULL; }
    return true;
}
static bool vfs_super_is_live_locked(const vfs_super_t *sb) { return sb == &super && online && sb->state != VFS_SUPER_DEAD; }
bool vfs_super_is_live(const vfs_super_t *sb) { return vfs_super_is_live_locked(sb); }
bool vfs_node_is_live(const vfs_node_t *node) { return node && vfs_super_is_live(node->super); }
vfs_node_t *vfs_get_root_node(void) { return &root; }
void vfs_super_release(vfs_super_t *sb) { assert(sb == &super && sb->reference_count > 1U); sb->reference_count--; }
static vfs_mount_t *vfs_find_mount_for_node_locked(vfs_node_t *node) { (void)node; return NULL; }
static bool vfs_handle_authorized(const vfs_file_handle_t *handle, u32 access) { (void)handle; (void)access; return false; }
bool vfs_directory_child_mutation_allowed(const vfs_node_t *parent, const vfs_node_t *child) {
    return vfs_check_access(parent, VFS_ACCESS_WRITE) && (!child || vfs_check_access(child, VFS_ACCESS_WRITE));
}
int vfs_recovery_pin_finish_handoff(u64 pin) {
    assert(pin && !operation_held && pin_active && !pin_unresolved);
    if (!pin_handoff(&super, pin_context)) {
        assert(vfs_recovery_pin_mark_unresolved(pin) == VFS_OK); return VFS_ERR_UNCERTAIN;
    }
    pin_active=false; pin_handoff=NULL; pin_context=NULL; return VFS_OK;
}
typedef struct { char name[VFS_MOUNT_PATH_MAX]; u64 inode; } vfs_mount_dirent_snapshot_t;
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
    'vfs_mount_parent_matches', 'vfs_find_mount_child_locked', 'vfs_directory_has_mount_children',
    'vfs_resolve_mount', 'vfs_finddir_locked', 'vfs_finddir_internal_checked', 'vfs_finddir_internal',
    'vfs_path_cstr_length', 'vfs_path_validate_n', 'vfs_lookup_internal', 'vfs_lookup', 'vfs_finddir',
    'vfs_open_node_internal', 'vfs_open_node', 'vfs_open', 'vfs_close', 'vfs_handle_valid', 'vfs_read_dispatch', 'vfs_file_read_internal', 'vfs_file_read_status', 'vfs_file_read',
    'vfs_operation_finish_pin', 'vfs_super_operation_complete', 'vfs_node_operation_finish', 'vfs_write_dispatch', 'vfs_file_write_status', 'vfs_write', 'vfs_read_status', 'vfs_read',
    'vfs_readdir_driver_checked', 'vfs_readdir_internal', 'vfs_readdir_checked',
    'vfs_node_register_created', 'vfs_node_unregister_created', 'vfs_create_node', 'vfs_create',
    'vfs_unlink_locked', 'vfs_unlink',
)

def seed_image(path):
    data = bytearray(path.read_bytes())
    get32 = lambda off: struct.unpack_from('<I', data, off)[0]
    heap, first, fat = get32(88), get32(96), get32(80)*512
    cluster_bytes = 1 << (data[108]+data[109])
    address = lambda c: heap*512+(c-2)*cluster_bytes
    root_at = address(first)
    metadata = [bytes(data[p:p+32]) for p in range(root_at, root_at+cluster_bytes, 32)
                if data[p] in (0x81, 0x82, 0x83)]
    bitmap = next(struct.unpack_from('<I', entry, 20)[0] for entry in metadata if entry[0] == 0x81)
    upcase = next(entry for entry in metadata if entry[0] == 0x82)
    raw = data[address(struct.unpack_from('<I', upcase, 20)[0]):]
    length = struct.unpack_from('<Q', upcase, 24)[0]
    table = []; at = 0
    while at < length:
        unit = struct.unpack_from('<H', raw, at)[0]; at += 2
        if unit == 65535 and len(table) != 65535:
            run = struct.unpack_from('<H', raw, at)[0]; at += 2
            table.extend(range(len(table), len(table)+run))
        else: table.append(unit)
    assert len(table) == 65536
    def allocate(payload, chained=False):
        c = next(c for c in range(2, get32(92)+2)
                 if not data[address(bitmap)+(c-2)//8] & (1 << ((c-2)%8)))
        data[address(bitmap)+(c-2)//8] |= 1 << ((c-2)%8)
        data[address(c):address(c)+len(payload)] = payload
        if chained: struct.pack_into('<I', data, fat+c*4, 0xffffffff)
        return c
    rotate = lambda n: (n >> 1) | ((n & 1) << 15)
    def entry(name, attributes, c, size, chained=False):
        utf16 = name.encode('utf-16-le'); units = struct.unpack('<'+'H'*(len(utf16)//2), utf16)
        count = 2+(len(units)+14)//15; result = bytearray(count*32)
        result[0]=0x85; result[1]=count-1; struct.pack_into('<H', result, 4, attributes)
        stamp=(46<<25)|(10<<21)|(14<<16)|(12<<11)
        for offset in (8, 12, 16): struct.pack_into('<I', result, offset, stamp)
        result[22:25]=b'\x80'*3
        result[32]=0xc0; result[33]=1 if chained else 3; result[35]=len(units)
        struct.pack_into('<Q', result, 40, size); struct.pack_into('<I', result, 52, c)
        struct.pack_into('<Q', result, 56, size)
        hashed=0
        for i, unit in enumerate(units):
            upper=table[unit]; hashed=(rotate(hashed)+(upper & 255)) & 65535
            hashed=(rotate(hashed)+(upper >> 8)) & 65535
            offset=(2+i//15)*32; result[offset]=0xc1
            struct.pack_into('<H', result, offset+2+2*(i%15), unit)
        struct.pack_into('<H', result, 36, hashed)
        checksum=0
        for i, byte in enumerate(result):
            if i not in (2,3): checksum=(rotate(checksum)+byte) & 65535
        struct.pack_into('<H', result, 2, checksum)
        return result
    data[root_at:root_at+cluster_bytes]=bytes(cluster_bytes)
    data[root_at:root_at+32*len(metadata)]=b''.join(metadata)
    for at in range(32*len(metadata), 480, 32): data[root_at+at]=0x05
    at=480
    for directory in (False, True):
        for combination in range(8):
            attributes=4 | (16 if directory else 0) | (combination & 3) | (32 if combination & 4 else 0)
            name=('Système' if directory else 'Système.txt') if not combination else f'{"dir" if directory else "file"}{combination}'
            c=allocate(bytes([65+combination]))
            if directory:
                child=entry('Résumé.txt', attributes & ~16, c, 1)
                chained=bool(combination & 2); c=allocate(child, chained)
            else: chained=False
            record=entry(name, attributes, c, cluster_bytes if directory else 1, chained)
            data[root_at+at:root_at+at+len(record)]=record; at += len(record)
    for name, attributes, payload in [('é'*128, 4, b'U'), ('later.txt', 0, b'L')]:
        record=entry(name, attributes, allocate(payload), len(payload))
        data[root_at+at:root_at+at+len(record)]=record; at += len(record)
    assert at+32 <= cluster_bytes
    count=get32(92)
    used=sum(bool(data[address(bitmap)+i//8] & (1 << (i%8))) for i in range(count))
    data[112]=used*100//count
    path.write_bytes(data)

def seed_boundary(path, span=1, malformed=False, fragmented=False):
    data = bytearray(path.read_bytes())
    get32 = lambda at: struct.unpack_from('<I', data, at)[0]
    count, heap, root_cluster = get32(92), get32(88), get32(96)
    cluster = 1 << (data[108] + data[109])
    address = lambda c: heap*512 + (c-2)*cluster
    root_at = address(root_cluster)
    bitmap = next(get32(at+20) for at in range(root_at, root_at+cluster, 32) if data[at] == 0x81)
    assert bitmap == 2  # Independent formatter owns the first valid cluster.
    start = next(at for at in range(root_at, root_at+cluster, 32) if data[at] == 0)
    last = count+1; first = last-span+1
    allocated = list(range(first, last+1))
    if fragmented:
        first = next(c for c in range(2, count+2) if not data[address(bitmap)+(c-2)//8] & (1 << ((c-2)%8)))
        allocated = [first, last]
    for c in allocated:
        data[address(bitmap)+(c-2)//8] |= 1 << ((c-2)%8)
        data[address(c):address(c)+cluster] = b'B'*cluster
    length = 1 if span == 1 else cluster+1
    if malformed: length = cluster+1
    entry = bytearray(96); entry[0]=0x85; entry[1]=2; entry[4]=32
    stamp=(46<<25)|(10<<21)|(14<<16)
    for at in (8,12,16): struct.pack_into('<I',entry,at,stamp)
    entry[22:25]=bytes([128])*3; entry[32]=0xc0; entry[33]=1 if fragmented else 3
    name='boundary.txt'; entry[35]=len(name)
    struct.pack_into('<Q',entry,40,length); struct.pack_into('<I',entry,52,first); struct.pack_into('<Q',entry,56,length)
    entry[64]=0xc1; rotate=lambda n:(n>>1)|((n&1)<<15); hashed=0
    for i,ch in enumerate(name):
        hashed=(rotate(hashed)+ord(ch.upper()))&65535; hashed=rotate(hashed)
        struct.pack_into('<H',entry,66+i*2,ord(ch))
    struct.pack_into('<H',entry,36,hashed); checksum=0
    for i,byte in enumerate(entry):
        if i not in (2,3): checksum=(rotate(checksum)+byte)&65535
    struct.pack_into('<H',entry,2,checksum); data[start:start+96]=entry
    if fragmented:
        fat=get32(80)*512
        struct.pack_into('<I',data,fat+first*4,last); struct.pack_into('<I',data,fat+last*4,0xffffffff)
    used=sum(bool(data[address(bitmap)+i//8] & (1 << (i%8))) for i in range(count))
    data[112]=used*100//count; path.write_bytes(data)

compiler = sys.argv[1] if len(sys.argv)>1 else 'gcc'
with tempfile.TemporaryDirectory(prefix='mangrove-exfat-system-') as temporary:
    directory=Path(temporary); binary=directory/'system'
    (directory/'vfs_system_visibility.inc').write_text(dependencies+''.join(function(n) for n in names))
    subprocess.run([compiler, '-O1', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-Wno-type-limits', '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
        *dead_strip_flags(), '-I'+str(directory), '-I'+str(root/'kernel/include'), '-I'+str(root/'include'),
        '-idirafter', str(root/'libc/include'), str(root/'tests/exfat_system_test.c'),
        str(root/'libc/src/utf8.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    boundary=directory/'boundary'
    subprocess.run([compiler, '-O1', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-Wno-type-limits', '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
        *dead_strip_flags(), '-I'+str(directory), '-I'+str(root/'kernel/include'), '-I'+str(root/'include'),
        '-idirafter', str(root/'libc/include'), str(root/'tests/exfat_boundary_test.c'),
        str(root/'libc/src/utf8.c'), '-o', str(boundary)], check=True)
    subprocess.run([str(boundary)], check=True)
    search=os.environ.get('PATH', '')+os.pathsep+'/usr/sbin'
    mkfs, fsck=(shutil.which(n, path=search) for n in ('mkfs.exfat', 'fsck.exfat'))
    if not mkfs or not fsck:
        print('SKIP independent SYSTEM image checks: install exfatprogs (mkfs.exfat and fsck.exfat).')
    else:
        image=directory/'imported.img'; output=directory/'updated.img'
        with image.open('wb') as stream: stream.truncate(12000*512)
        subprocess.run([mkfs, '-c', '4096', '-L', 'META', str(image)], check=True)
        seed_image(image)
        subprocess.run([fsck, '-n', str(image)], check=True)
        original_hash=hashlib.sha256(image.read_bytes()).digest()
        subprocess.run([str(binary), str(image), str(output)], check=True)
        subprocess.run([fsck, '-n', str(output)], check=True)
        output_hash=hashlib.sha256(output.read_bytes()).digest()
        subprocess.run([str(binary), '--readback', str(output)], check=True)
        assert original_hash == hashlib.sha256(image.read_bytes()).digest()
        assert output_hash == hashlib.sha256(output.read_bytes()).digest()
        print('Independent SYSTEM files/directories, split entry sets, Unicode and full VFS lifecycle verified.')

        for kind,span,bad,fragmented in (('last',1,False,False),('extent',2,False,False),('fragmented',2,False,True),('overflow',1,True,False)):
            original=directory/(kind+'.img'); updated=directory/(kind+'-updated.img')
            with original.open('wb') as stream: stream.truncate(12000*512)
            subprocess.run([mkfs,'-c','4096',str(original)],check=True)
            seed_boundary(original,span,bad,fragmented)
            digest=hashlib.sha256(original.read_bytes()).digest()
            check=subprocess.run([fsck,'-n',str(original)],check=False)
            if bad:
                assert check.returncode != 0
                subprocess.run([str(boundary),'--invalid',str(original)],check=True)
            else:
                assert check.returncode == 0
                subprocess.run([str(boundary),str(original),str(updated)],check=True)
                subprocess.run([fsck,'-n',str(updated)],check=True)
            assert digest == hashlib.sha256(original.read_bytes()).digest()
        print('Independent final-cluster contiguous/fragmented lifecycle and overflow rejection passed.')
