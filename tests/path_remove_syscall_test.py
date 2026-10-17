#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production path-remove dispatcher with controlled VFS state."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "kernel/src/syscall.c").read_text()


def block(start):
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[brace:end]


def function(name):
    match = re.search(r"^static [^\n;{}]*\b" + name + r"\s*\(", source, re.M)
    assert match, name
    brace = source.index("{", match.end())
    return source[match.start():brace] + block(brace) + "\n"


case = source.index("case SYSCALL_PATH_REMOVE:")
dispatch_case = "case SYSCALL_PATH_REMOVE:" + block(case)
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <vfs.h>
#include <mangrove_errors.h>
#include <identity.h>
typedef struct {unsigned unused;} process_t;
typedef struct {u64 rax,rdi;} syscall_frame_t;
static process_t process;
static vfs_super_t super;
static vfs_node_t parent,node;
struct vfs_location {vfs_node_t *node;unsigned references;};
static vfs_location_t location={.node=&parent};
static bool trailing,permission,valid_path;
static unsigned unlinks,directories;
static unsigned authorizations;
static int lookup_result;
static process_t *process_current(void){return &process;}
static int syscall_lookup_parent(process_t *p,const char *path,
    vfs_location_t **out,char *name,usize capacity,bool *out_trailing_separator){
    assert(p==&process && capacity==256);
    if(!valid_path || (strcmp(path,"/item") && strcmp(path,"/item/")))
        return VFS_ERR_INVALID_PARAM;
    *out=&location;strcpy(name,"item");
    if(out_trailing_separator)*out_trailing_separator=trailing;
    return VFS_OK;
}
vfs_node_t *vfs_location_node(vfs_location_t *p){return p?p->node:NULL;}
void vfs_location_release(vfs_location_t *p){(void)p;}
bool vfs_check_access(const vfs_node_t *n,u32 mode){
    assert(n==&parent && mode==VFS_ACCESS_WRITE);return permission;
}
int vfs_finddir_checked(vfs_node_t *dir,const char *name,vfs_node_t **out){
    assert(dir==&parent && !strcmp(name,"item"));*out=NULL;
    if(lookup_result)return lookup_result;
    *out=&node;return VFS_OK;
}
int vfs_finddir_trusted_checked(vfs_node_t *dir,const char *name,
    vfs_node_t **out){return vfs_finddir_checked(dir,name,out);}
int vfs_unlink_expected(vfs_node_t *p,const char *name,vfs_super_t *sb,u64 inode){
    assert(p==&parent && !strcmp(name,"item") && sb==&super && inode==node.inode);
    unlinks++;return VFS_OK;
}
int vfs_rmdir_expected(vfs_node_t *p,const char *name,vfs_super_t *sb,u64 inode){
    assert(p==&parent && !strcmp(name,"item") && sb==&super && inode==node.inode);
    directories++;return VFS_OK;
}
int vfs_unlink_expected_authorized(vfs_node_t *p,const char *name,
    vfs_super_t *sb,u64 inode){
    return vfs_unlink_expected(p,name,sb,inode);
}
int vfs_rmdir_expected_authorized(vfs_node_t *p,const char *name,
    vfs_super_t *sb,u64 inode){
    return vfs_rmdir_expected(p,name,sb,inode);
}
static bool syscall_admin_credentials(process_t *p,process_credentials_t *c){
    assert(p==&process && c);return true;
}
static int syscall_admin_parent(process_t *p,const char *path,
    const process_credentials_t *c,vfs_location_t **out,char *name,usize capacity,
    bool *out_trailing_separator){
    assert(c);return syscall_lookup_parent(p,path,out,name,capacity,
                                           out_trailing_separator);
}
static bool syscall_admin_parent_allowed(vfs_node_t *p,
    const process_credentials_t *c){assert(p==&parent&&c);return permission;}
bool vfs_administrator_override_allowed(const vfs_node_t *n,
    const process_credentials_t *c){assert(n&&c);return true;}
static int syscall_admin_authorize(process_t *p,const char *text,
    process_credentials_t *c){assert(p==&process&&text&&c);authorizations++;return MG_OK;}
static bool syscall_admin_revalidate(vfs_location_t *p,
    process_credentials_t *c){assert(p==&location&&c);return permission;}
static void syscall_fail(syscall_frame_t *f,i64 r){f->rax=(u64)r;}
static void reset(void){
    memset(&super,0,sizeof(super));memset(&parent,0,sizeof(parent));
    memset(&node,0,sizeof(node));parent.super=node.super=&super;
    parent.type=VFS_TYPE_DIRECTORY;node.type=VFS_TYPE_FILE;node.inode=11;
    location.node=&parent;location.references=0;trailing=false;
    permission=valid_path=true;lookup_result=VFS_OK;unlinks=directories=authorizations=0;
}
'''
code = prefix + function("syscall_vfs_error") + function("syscall_admin_same_node") + \
    function("syscall_admin_remove") + \
    "static void dispatch_remove(syscall_frame_t *frame) { switch (frame->rax) {" + \
    dispatch_case + " default: assert(0); } }\n" + r'''
int main(void){
    syscall_frame_t frame={.rax=SYSCALL_PATH_REMOVE,.rdi=(u64)"/item"};
    reset();dispatch_remove(&frame);
    assert((i64)frame.rax==MG_OK&&unlinks==1&&!directories);
    reset();trailing=true;frame.rax=SYSCALL_PATH_REMOVE;frame.rdi=(u64)"/item/";
    dispatch_remove(&frame);
    assert((i64)frame.rax==MG_ERR_NOT_DIRECTORY&&!unlinks&&!directories);
    reset();trailing=true;node.type=VFS_TYPE_DIRECTORY;
    frame.rax=SYSCALL_PATH_REMOVE;frame.rdi=(u64)"/item/";dispatch_remove(&frame);
    assert((i64)frame.rax==MG_OK&&!unlinks&&directories==1);
    reset();permission=false;trailing=true;frame.rax=SYSCALL_PATH_REMOVE;
    frame.rdi=(u64)"/item/";dispatch_remove(&frame);
    assert((i64)frame.rax==MG_ERR_ACCESS_DENIED&&!unlinks&&!directories);
    reset();lookup_result=VFS_ERR_IO;frame.rax=SYSCALL_PATH_REMOVE;
    frame.rdi=(u64)"/item";dispatch_remove(&frame);
    assert((i64)frame.rax==MG_ERR_IO&&!unlinks&&!directories);
    reset();trailing=true;
    assert(syscall_admin_remove(&process,"/item/")==MG_ERR_NOT_DIRECTORY &&
           !unlinks&&!directories&&!authorizations);
    reset();trailing=true;node.type=VFS_TYPE_DIRECTORY;
    assert(syscall_admin_remove(&process,"/item/")==MG_OK &&
           !unlinks&&directories==1&&authorizations==1);
    reset();
    assert(syscall_admin_remove(&process,"/item")==MG_OK &&
           unlinks==1&&!directories&&authorizations==1);
    puts("path removal preserves trailing-directory semantics and failure atomicity PASS");
}
'''
with tempfile.TemporaryDirectory(prefix="mangrove-path-remove-") as work:
    work = Path(work)
    c_file, binary = work / "fixture.c", work / "fixture"
    c_file.write_text(code)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else "gcc", "-std=gnu11",
        "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I"+str(root/"kernel/include"), "-I"+str(root/"include"),
        "-idirafter", str(root/"libc/include"), str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
