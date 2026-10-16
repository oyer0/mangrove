#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production native inspection, credential predicate and VFS pin registry.
Controlled user mappings, identity registry, host transport/allocator/scheduler.
No transaction is fabricated as a successfully recovered filesystem mutation.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def function(path, name):
    source = (ROOT / path).read_text()
    for match in re.finditer(r'^(?:static )?[^\n;{}]*\b' + name + r'\s*\(', source, re.M):
        brace = source.find('{', match.end())
        if ';' in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end] + '\n'
    raise AssertionError(name)

PREFIX = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <process.h>
#include <mg/inspection.h>
#include "mfqs/host.h"
#include "storage/mgfs.c"
static process_t caller;
static bool credentials_available=true;
static const void *unwritable;
bool process_get_credentials(const process_t *p,process_credentials_t *out)
{ if(p!=&caller || !credentials_available)return false;*out=p->credentials;return true; }
bool identity_registry_lookup_uid(mg_uid_t uid,user_identity_t *out)
{ if(uid!=7)return false;memset(out,0,sizeof(*out));out->uid=uid;out->role=MG_IDENTITY_ROLE_ADMIN;return true; }
static bool syscall_user_buffer_valid(const void *p,u64 n){return p && n;}
bool vmm_user_range_writable(const void *p,usize n){return p && n && p!=unwritable;}
'''
SUFFIX = r'''
int main(int argc,char **argv)
{
    assert(argc==2 && mfqs_media_open(argv[1]));
    vfs_init();assert(mgfs_init()==VFS_OK);
    assert(vfs_mount_root("mgfs",mfqs_device())==VFS_OK);
    vfs_super_t *sb=vfs_get_root_node()->super;
    mg_recovery_owner_info_t result[MG_RECOVERY_OWNER_MAX],before[MG_RECOVERY_OWNER_MAX];
    u32 count=99;
    mg_recovery_owner_snapshot_request_t request={MG_RECOVERY_OWNER_MAX,result,&count};
    caller.credentials=(process_credentials_t){.uid=8,.role=MG_IDENTITY_ROLE_REGULAR};
    assert(syscall_recovery_owners(&caller,&request)==MG_ERR_ACCESS_DENIED && count==99);
    caller.credentials.uid=7;caller.credentials.role=MG_IDENTITY_ROLE_ADMIN;
    assert(syscall_recovery_owners(&caller,&request)==MG_OK && !count);
    u64 pins[VFS_MAX_RECOVERY_PINS];
    assert(vfs_super_operation_lock(sb));
    for(unsigned i=0;i<VFS_MAX_RECOVERY_PINS;i++)assert(vfs_recovery_pin_begin(sb,&pins[i])==VFS_OK);
    u64 extra=0;assert(vfs_recovery_pin_begin(sb,&extra)==VFS_ERR_NO_SPACE && !extra);
    vfs_super_operation_unlock(sb);
    memset(result,0xa5,sizeof(result));memcpy(before,result,sizeof(result));
    request.result_capacity=1;
    assert(syscall_recovery_owners(&caller,&request)==MG_ERR_BUFFER_TOO_SMALL && count==32);
    assert(!memcmp(before,result,sizeof(result)));
    request.out_count=(u32 *)result;
    assert(syscall_recovery_owners(&caller,&request)==MG_ERR_BAD_ARGUMENT);
    assert(!memcmp(before,result,sizeof(result)));
    request.out_count=&count;
    request.result_capacity=MG_RECOVERY_OWNER_MAX;
    assert(syscall_recovery_owners(&caller,&request)==MG_OK && count==32);
    for(unsigned i=0;i<count;i++){
        assert(result[i].owner_id && result[i].state==MG_RECOVERY_OWNER_ACTIVE);
        assert(result[i].instance_id==(BLOCK_DEVICE_ID_BASE|(mfqs_device()->id+1)));
        assert(result[i].physical_instance_id==result[i].instance_id);
        assert(result[i].flags==(MG_RECOVERY_DEVICE_ONLINE|MG_RECOVERY_PHYSICAL_ONLINE));
        for(unsigned j=0;j<i;j++)assert(result[j].owner_id!=result[i].owner_id);
    }
    unwritable=result;
    assert(syscall_recovery_owners(&caller,&request)==MG_ERR_BAD_ARGUMENT);
    unwritable=NULL;
    for(unsigned i=1;i<32;i++)assert(vfs_recovery_pin_finish(pins[i])==VFS_OK);
    assert(vfs_recovery_pin_mark_unresolved(pins[0])==VFS_OK);
    assert(syscall_recovery_owners(&caller,&request)==MG_OK && count==1);
    assert(result[0].owner_id==pins[0] && result[0].state==MG_RECOVERY_OWNER_UNRESOLVED);
    mfqs_device()->online=false;vfs_device_removed(mfqs_device());
    assert(syscall_recovery_owners(&caller,&request)==MG_OK && count==1);
    assert(result[0].owner_id==pins[0] && result[0].flags==MG_RECOVERY_OFFLINE_RETAINED);
    /* Authorized kernel-created storage service; no human privilege spoof. */
    caller.credentials=(process_credentials_t){.uid=MG_UID_SYSTEM,.role=MG_IDENTITY_ROLE_SYSTEM,
        .service_privileges=IDENTITY_PRIVILEGE_MASK(IDENTITY_PRIVILEGE_MANAGE_STORAGE)};
    assert(syscall_recovery_owners(&caller,&request)==MG_OK && count==1);
    caller.credentials.service_privileges=0;
    assert(syscall_recovery_owners(&caller,&request)==MG_ERR_ACCESS_DENIED);
    mfqs_media_close();
    puts("recovery inspection: actual registry, credential predicate, capacity, immutable/offline identities PASS");
    return 0; /* Deliberately unresolved registry owner remains; LSan disabled. */
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-recovery-inspection-') as work:
    work = Path(work)
    source, binary, image = work / 'test.c', work / 'test', work / 'media.img'
    source.write_text(PREFIX + ''.join(function('kernel/src/identity.c', name) for name in
        ('identity_credentials_valid','identity_credentials_is_system','identity_credentials_has_privilege')) +
        function('kernel/src/syscall.c','syscall_vfs_error') +
        function('kernel/src/syscall.c','syscall_recovery_owners') + SUFFIX)
    compiler = sys.argv[1] if len(sys.argv)>1 else 'gcc'
    flags = [compiler,'-std=gnu11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function',
             '-fsanitize=address,undefined','-fno-sanitize-recover=all',
             '-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel/src'),
             '-I'+str(ROOT/'include'),'-idirafter',str(ROOT/'libc/include'),'-I'+str(ROOT/'tests')]
    replacements = ('process_get_credentials','identity_credentials_valid',
                    'identity_credentials_has_privilege','identity_registry_lookup_uid')
    subprocess.run(flags + ['-D'+name+'=unused_'+name for name in replacements] +
        ['-c',str(ROOT/'tests/mfqs/host.c'),'-o',str(work/'host.o')],check=True)
    subprocess.run(flags + [str(source),str(ROOT/'kernel/src/vfs.c'),str(work/'host.o'),
        str(ROOT/'libc/src/utf8.c'),'-o',str(binary)],check=True)
    subprocess.run([str(ROOT/'build/mkmgfs'),'--blocks','3200','--uuid','11111111-2222-3333-4444-555555555555','--format-time-ns','1',str(image)],check=True)
    subprocess.run([str(binary),str(image)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    subprocess.run([str(ROOT/'build/mgfsck'),str(image)],check=True)
