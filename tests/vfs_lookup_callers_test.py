#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production mount/account/configuration admission with controlled lookup errors."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def function(path,name):
    source=(ROOT/path).read_text()
    match=re.search(r'^(?:static )?[^\n;{}]*\b'+name+r'\s*\(',source,re.M)
    assert match,name
    brace=source.index('{',match.end());end=brace+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]+'\n'
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vfs.h>
#define NAMESPACE_SCOPE(failure) ((void)0)
#define IDENTITY_ACCOUNT_DIR_PATH "/sys/accounts"
static vfs_node_t directory={.type=VFS_TYPE_DIRECTORY};
static mutex_t vfs_mount_lock;
static bool locked;
static int full_result,results[3];
static unsigned queries,creates,writes,mounts;
static const char default_network_config[]="configuration";
bool mutex_lock(mutex_t *m){assert(m==&vfs_mount_lock && !locked);locked=true;return true;}
bool mutex_unlock(mutex_t *m){assert(m==&vfs_mount_lock && locked);locked=false;return true;}
bool vfs_node_is_live(const vfs_node_t *p){return p==&directory;}
static bool vfs_device_already_mounted(block_device_t *p){(void)p;return false;}
static vfs_mount_t *vfs_find_mount_child(vfs_node_t *p,const char *n){assert(p==&directory && n);return NULL;}
static int vfs_mount_instance(vfs_node_t *p,const char *n,vfs_node_t *node,const char *path,
 const char *fs,block_device_t *d,vfs_mount_role_t role,bool ro)
{(void)p;(void)n;(void)node;(void)path;(void)fs;(void)d;(void)role;(void)ro;mounts++;return VFS_OK;}
vfs_node_t *vfs_get_root_node(void){return &directory;}
int vfs_lookup_trusted(const char *p,vfs_node_t **out){assert(p);*out=full_result==VFS_OK?&directory:NULL;return full_result;}
int vfs_finddir_checked(vfs_node_t *p,const char *n,vfs_node_t **out)
{assert(p==&directory && n && queries<3);int r=results[queries++];*out=r==VFS_OK?&directory:NULL;return r;}
int vfs_finddir_trusted_checked(vfs_node_t *p,const char *n,vfs_node_t **out)
{return vfs_finddir_checked(p,n,out);}
int vfs_mkdir(vfs_node_t *p,const char *n,vfs_node_t **out)
{assert(p==&directory && n);creates++;*out=&directory;return VFS_OK;}
int vfs_mkdir_owned(vfs_node_t *p,const char *n,u32 uid,u32 perms,vfs_node_t **out)
{(void)uid;(void)perms;return vfs_mkdir(p,n,out);}
int vfs_create(vfs_node_t *p,const char *n,vfs_node_t **out){return vfs_mkdir(p,n,out);}
u64 vfs_write(vfs_node_t *p,u64 offset,u64 size,const void *data)
{assert(p==&directory && !offset && data);writes++;return size;}
static void reset(void){assert(!locked);queries=creates=writes=mounts=0;full_result=VFS_ERR_NOT_FOUND;
 for(unsigned i=0;i<3;i++)results[i]=VFS_OK;}
'''
for path,name in [('kernel/src/vfs.c','vfs_mount_path'),('kernel/src/identity.c','account_storage_directory'),
                  ('kernel/src/net/config.c','ensure_directory'),('kernel/src/net/config.c','create_default_config')]:
    code+=function(path,name)
code+=r'''
int main(void){
 vfs_node_t *node=NULL;
 reset();results[0]=VFS_ERR_IO;
 assert(vfs_mount_path(&directory,"volume","/vol/volume","mgfs",NULL,VFS_MOUNT_ROLE_VOLUME,false)==VFS_ERR_IO);
 assert(!locked && !mounts && !creates && !writes);
 reset();results[0]=VFS_ERR_NOT_FOUND;
 assert(vfs_mount_path(&directory,"volume","/vol/volume","mgfs",NULL,VFS_MOUNT_ROLE_VOLUME,false)==VFS_OK);
 assert(!locked && mounts==1);
 reset();full_result=VFS_ERR_IO;assert(account_storage_directory(&node)==VFS_ERR_IO && !queries && !creates);
 for(unsigned at=0;at<2;at++){
  reset();results[at]=VFS_ERR_IO;assert(account_storage_directory(&node)==VFS_ERR_IO && !creates);
 }
 reset();results[0]=results[1]=VFS_ERR_NOT_FOUND;
 assert(account_storage_directory(&node)==VFS_OK && node==&directory && creates==2);
 reset();results[0]=VFS_ERR_IO;assert(!ensure_directory(&directory,"conf",&node) && !creates);
 reset();results[0]=VFS_ERR_NOT_FOUND;assert(ensure_directory(&directory,"conf",&node) && creates==1);
 for(unsigned at=0;at<3;at++){
  reset();results[at]=VFS_ERR_IO;assert(!create_default_config() && !creates && !writes);
 }
 reset();assert(create_default_config() && !creates && !writes);
 reset();results[2]=VFS_ERR_NOT_FOUND;assert(create_default_config() && creates==1 && writes==1);
 puts("Checked lookup callers: mount/account/configuration fail closed; confirmed absence still creates");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-lookup-callers-') as temp:
    source=Path(temp)/'test.c';binary=Path(temp)/'test';source.write_text(code)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-std=gnu11','-O1','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'kernel/include'),
        '-I'+str(ROOT/'include'),'-idirafter',str(ROOT/'libc/include'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
