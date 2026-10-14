#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production label, storage/native dispatch and registry; disposable FAT32 media."""
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
vfs = (root/'kernel/src/vfs.c').read_text()
fmt = (root/'kernel/src/storage/format.c').read_text()
sc = (root/'kernel/src/syscall.c').read_text()
def function(source, name):
    for m in re.finditer(r'^(?:static )?[^\n;{}]*\b'+name+r'\s*\(', source, re.M):
        brace = source.find('{', m.end())
        if ';' in source[m.end():brace]: continue
        end, depth = brace+1, 1
        while depth:
            depth += (source[end]=='{')-(source[end]=='}'); end+=1
        return source[m.start():end]+'\n'
    raise AssertionError(name)
names = ('vfs_device_range','vfs_device_already_mounted','vfs_retarget_nodes_locked',
    'vfs_super_is_live_locked','vfs_super_mark_dispose_locked','vfs_super_destroy','vfs_super_release',
    'vfs_recovery_private_begin','vfs_recovery_private_release','vfs_recovery_pin_mark_unresolved',
    'vfs_recovery_pin_finish_handoff','vfs_recovery_pins_snapshot','vfs_format_preflight')
registry = vfs[vfs.index('typedef struct vfs_recovery_pin {'):vfs.index('static vfs_mount_t *vfs_find_mount_child')]
registry += '\nstatic vfs_mount_t mount_table[VFS_MAX_MOUNTS];\nstatic vfs_super_t dead_super;\nstatic spinlock_t vfs_metadata_lock;\nstatic mutex_t vfs_mount_lock;\n'
case_start = sc.index('        case SYSCALL_STORAGE_LABEL: {')
case_end = sc.index('        case SYSCALL_STORAGE_GPT_INFO:',case_start)
native = r'''
typedef struct { bool storage_management_session; u32 storage_authorized_operation; } label_process_t;
#define process_t label_process_t
static label_process_t requester;
static label_process_t *process_current(void) { return &requester; }
static bool syscall_is_diskutil(label_process_t *p) { return p == &requester; }
static bool syscall_user_buffer_valid(const void *p, usize n) { return p && n; }
static bool syscall_fixed_text_valid(const char *p, usize n, bool empty) {
    return p && (empty || *p) && memchr(p,0,n);
}
block_device_t *block_get_device_by_public_id(u64 id) { return id==(BLOCK_DEVICE_ID_BASE | (device.id+1U)) ? &device : NULL; }
static bool syscall_storage_device_matches(block_device_t *d, u64 parent) { return d==&device && !parent; }
static bool replace_before_dispatch;
static bool syscall_storage_system_managed(block_device_t *d) { (void)d; if (replace_before_dispatch) device.id++; return false; }
#define SYSCALL_STORAGE_LABEL 74U
typedef struct { u64 rax, rdi; } label_frame_t;
static void syscall_fail(label_frame_t *f, i64 result) { f->rax=(u64)result; }
'''+function(sc,'syscall_vfs_error')+'\nstatic void label_syscall(label_frame_t *frame) { switch(frame->rax) {\n'+sc[case_start:case_end]+'default: assert(0); } }\n#undef process_t\n'
search = os.environ.get('PATH','')+os.pathsep+'/usr/sbin'
tools = {name:shutil.which(name,path=search) for name in ('mkfs.fat','fsck.fat','mcopy','mdir')}
if not all(tools.values()):
    raise SystemExit('FAT32 label interoperability requires dosfstools and mtools: '+', '.join(n for n,p in tools.items() if not p))
compiler = sys.argv[1] if len(sys.argv)>1 else 'gcc'
with tempfile.TemporaryDirectory(prefix='mangrove-fat32-label-') as temporary:
    directory = Path(temporary)
    (directory/'vfs_fat32_label_owner.inc').write_text(registry+'\n'.join(function(vfs,n) for n in names))
    (directory/'fat32_label_dispatch.inc').write_text(function(fmt,'storage_set_filesystem_label_for_id')+function(fmt,'storage_set_filesystem_label')+native)
    binary=directory/'label'
    subprocess.run([compiler,'-O1','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-type-limits',
        '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined',*dead_strip_flags(),
        '-I'+str(directory),'-I'+str(root/'kernel/include'),'-I'+str(root/'include'),'-idirafter',str(root/'libc/include'),
        str(root/'tests/fat32_label_test.c'),'-o',str(binary)],check=True)
    for layout in ('existing','empty','marker','full','fragmented','nobackup'):
        image=directory/(layout+'.img'); output=directory/(layout+'-updated.img')
        with image.open('wb') as f: f.truncate(64*1024*1024)
        args=[tools['mkfs.fat'],'-F','32','-s','1']
        if layout=='existing': args+=['-n','OLDLABEL']
        subprocess.run(args+[str(image)],check=True)
        if layout in ('existing','marker'):
            neighbor=directory/'neighbor.bin'; neighbor.write_bytes(b'KEEP'*257)
            subprocess.run([tools['mcopy'],'-i',str(image),str(neighbor),'::KEEP.TXT'],check=True)
        if layout in ('marker','nobackup'):
            data=bytearray(image.read_bytes())
            if layout=='nobackup':
                struct.pack_into('<H',data,50,0)
            else:
                reserved=struct.unpack_from('<H',data,14)[0]
                heap=reserved+data[16]*struct.unpack_from('<I',data,36)[0]
                at=heap*512
                data[at+32+5]=0xa5; data[at+64+5]=0xa5
            image.write_bytes(data)
        if layout in ('full','fragmented'):
            data=bytearray(image.read_bytes()); reserved=struct.unpack_from('<H',data,14)[0]
            copies=data[16]; fat_size=struct.unpack_from('<I',data,36)[0]
            first=struct.unpack_from('<I',data,44)[0]; heap=reserved+copies*fat_size
            clusters=[first] if layout=='full' else [first,13]
            for j,c in enumerate(clusters):
                for copy in range(copies):
                    struct.pack_into('<I',data,(reserved+copy*fat_size)*512+c*4,clusters[j+1] if j+1<len(clusters) else 0x0fffffff)
                at=(heap+c-2)*512
                for i in range(16):
                    record=bytearray(32); record[:11]=f'F{j*16+i:07d}TXT'.encode();record[11]=0x20
                    data[at+i*32:at+(i+1)*32]=record
            for sector in (1,7):
                struct.pack_into('<I',data,sector*512+488,0xffffffff)
                struct.pack_into('<I',data,sector*512+492,0xffffffff)
            image.write_bytes(data)
        def check_image(path):
            result=subprocess.run([tools['fsck.fat'],'-n',str(path)],capture_output=True,text=True)
            print(result.stdout,end='')
            if layout=='nobackup':
                assert result.returncode in (0,1) and 'There is no backup boot sector.' in result.stdout
                assert 'Auto-creating backup boot block.' in result.stdout
                permitted=('fsck.fat ', 'There is no backup boot sector.', '  Auto-creating backup boot block.',
                    'Created backup of boot sector in sector ', 'Leaving filesystem unchanged.',str(path)+':')
                assert all(not line or line.startswith(permitted) for line in result.stdout.splitlines()),result.stdout
                print('Checker recommends an optional backup for the declared-absent layout; no repair applied.')
            else: assert result.returncode==0,result.stdout+result.stderr
        check_image(image)
        digest=hashlib.sha256(image.read_bytes()).digest()
        subprocess.run([str(binary),str(image),str(output)],check=True)
        check_image(output)
        failed_clear=Path(str(output)+'.failed-clear')
        check_image(failed_clear)
        assert failed_clear.read_bytes()==image.read_bytes()
        print('Failed-clear image is unchanged; native result remained UNCERTAIN with retained pin regardless of checker disposition.')
        subprocess.run([tools['mdir'],'-i',str(output),'::'],check=True)
        if layout in ('existing','marker'):
            copied=directory/'readback';subprocess.run([tools['mcopy'],'-i',str(output),'::KEEP.TXT',str(copied)],check=True)
            assert copied.read_bytes()==neighbor.read_bytes()
        assert digest==hashlib.sha256(image.read_bytes()).digest()
    print('Independent FAT32 label consistency, directory growth, readback and unchanged inputs passed.')

    for kind,expected in (('duplicate',-5),('oem',-6),('collision',-13),('tail',-5),('mirrors',-5),('no-space',-12)):
        image=directory/(kind+'.img')
        with image.open('wb') as f: f.truncate(64*1024*1024)
        subprocess.run([tools['mkfs.fat'],'-F','32','-s','1','-n','OLDLABEL',str(image)],check=True)
        data=bytearray(image.read_bytes()); reserved=struct.unpack_from('<H',data,14)[0]
        fat_size=struct.unpack_from('<I',data,36)[0]; heap=reserved+data[16]*fat_size
        cluster=struct.unpack_from('<I',data,44)[0]; at=(heap+cluster-2)*512
        if kind in ('duplicate','collision','oem'):
            record=bytearray(32); record[:11]=b'OTHER      ' if kind=='duplicate' else b'NEWLABEL   '
            record[11]=8 if kind=='duplicate' else 32
            if kind=='oem': record[0]=128
            data[at+32:at+64]=record
        if kind=='tail': data[at+96:at+107]=b'HIDDEN  TXT'; data[at+107]=32
        if kind=='mirrors': struct.pack_into('<I',data,(reserved+fat_size)*512+cluster*4,0)
        if kind=='no-space':
            for i in range(16):
                record=bytearray(32); record[:11]=f'F{i:07d}TXT'.encode(); record[11]=32
                data[at+i*32:at+(i+1)*32]=record
            count=(len(data)//512-heap)//data[13]
            for copy in range(data[16]):
                for c in range(2,count+2):
                    struct.pack_into('<I',data,(reserved+copy*fat_size)*512+c*4,0x0fffffff if c==cluster else 0x0ffffff7)
            for sector in (1,7):
                struct.pack_into('<I',data,sector*512+488,0xffffffff)
                struct.pack_into('<I',data,sector*512+492,0xffffffff)

        image.write_bytes(data); digest=hashlib.sha256(data).digest()
        subprocess.run([str(binary),'--reject',str(image),str(expected)],check=True)
        assert digest==hashlib.sha256(image.read_bytes()).digest()
    print('Duplicate labels, OEM ambiguity, namespace collisions, hidden tails and mirror corruption rejected unchanged.')
