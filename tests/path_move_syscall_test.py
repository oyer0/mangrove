#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute native and authorized move production paths with controlled VFS."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/syscall.c').read_text()

def block(start):
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[brace:end]

def function(name):
    match = re.search(r'^static [^\n;{}]*\b' + name + r'\s*\(', source, re.M)
    assert match, name
    brace = source.index('{', match.end())
    return source[match.start():brace] + block(brace) + '\n'

prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vfs.h>
#include <mg/credentials.h>
#include <mangrove_errors.h>
typedef struct {unsigned unused;} process_t;
typedef struct {u64 rax, rdi, rsi;} syscall_frame_t;
static process_t process;
static vfs_super_t super;
static vfs_node_t src, dst, child, alias, other;
static unsigned destination_mode, moves, authorizations;
static bool permitted, path_valid, replace_during_authorization;
static int operation_result, lookup_result;
static unsigned lookup_calls, lookup_at;
static process_t *process_current(void) { return &process; }
struct vfs_location {vfs_node_t *node;u32 references;};
static vfs_location_t source_location={.node=&src}, destination_location={.node=&dst};
void vfs_location_release(vfs_location_t *p){if(p){assert(p->references);p->references--;}}
vfs_node_t *vfs_location_node(vfs_location_t *p){return p ? p->node : NULL;}
static int syscall_lookup_parent(process_t *p,const char *path,
                                  vfs_location_t **out,char *name,usize capacity)
{
    assert(p==&process && capacity>=9);
    if(!path_valid)return VFS_ERR_INVALID_PARAM;
    *out=!strcmp(path,"source") ? &source_location : &destination_location;
    (*out)->references++;
    strcpy(name,!strcmp(path,"source") ? "file.txt" : "FILE.TXT");return VFS_OK;
}
vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name)
{
    if (dir == &src) { assert(!strcmp(name, "file.txt")); return &child; }
    assert(dir == &dst && !strcmp(name, "FILE.TXT"));
    return destination_mode == 1U ? &alias : destination_mode == 2U ? &other : NULL;
}
vfs_node_t *vfs_finddir_trusted(vfs_node_t *dir, const char *name)
{ return vfs_finddir(dir, name); }
int vfs_finddir_checked(vfs_node_t *dir,const char *name,vfs_node_t **out)
{ *out=NULL; lookup_calls++;
  if(lookup_result && (!lookup_at || lookup_calls==lookup_at)) return lookup_result;
  *out=vfs_finddir(dir,name); return *out ? VFS_OK : VFS_ERR_NOT_FOUND; }
int vfs_finddir_trusted_checked(vfs_node_t *dir,const char *name,vfs_node_t **out)
{ return vfs_finddir_checked(dir,name,out); }

bool vfs_check_access(const vfs_node_t *node, u32 permission)
{ assert((node == &src || node == &dst) && permission == VFS_ACCESS_WRITE); return permitted; }
int vfs_rename_expected(vfs_node_t *parent, const char *name,
    vfs_super_t *sb, u64 inode, vfs_node_t *destination, const char *new_name)
{
    assert(parent == &src && destination == &dst && sb == &super && inode == child.inode &&
           !strcmp(name, "file.txt") && !strcmp(new_name, "FILE.TXT"));
    moves++; return operation_result;
}
int vfs_rename_expected_authorized(vfs_node_t *parent, const char *name,
    vfs_super_t *sb, u64 inode, vfs_node_t *destination, const char *new_name)
{ return vfs_rename_expected(parent, name, sb, inode, destination, new_name); }
static bool syscall_admin_credentials(process_t *p, process_credentials_t *credentials)
{ assert(p == &process && credentials); return permitted; }
static int syscall_admin_parent(process_t *p,const char *path,
    const process_credentials_t *credentials,vfs_location_t **out,char *name,usize capacity)
{ assert(credentials);return syscall_lookup_parent(p,path,out,name,capacity); }
static bool syscall_admin_revalidate(vfs_location_t *p,process_credentials_t *credentials)
{ assert(p && credentials);return permitted; }
static bool syscall_admin_parent_allowed(vfs_node_t *parent,
    const process_credentials_t *credentials)
{ assert(parent && credentials); return permitted; }
bool vfs_administrator_override_allowed(const vfs_node_t *node,
    const process_credentials_t *credentials)
{ assert(node && credentials); return permitted; }
static int syscall_admin_authorize(process_t *p, const char *description,
    process_credentials_t *credentials)
{
    assert(p == &process && description && credentials);
    authorizations++;
    if (replace_during_authorization) destination_mode = 2U;
    return MG_OK;
}
static void syscall_fail(syscall_frame_t *frame, i64 result) { frame->rax = (u64)result; }
static void reset(void)
{
    assert(!source_location.references && !destination_location.references);
    memset(&super, 0, sizeof(super)); memset(&src, 0, sizeof(src)); memset(&dst, 0, sizeof(dst));
    memset(&child, 0, sizeof(child));
    src.super = dst.super = child.super = &super;
    src.type = dst.type = VFS_TYPE_DIRECTORY; child.type = VFS_TYPE_FILE;
    src.inode = 1U; dst.inode = 2U; child.inode = 3U;
    alias = other = child; other.inode++;
    destination_mode = moves = authorizations = 0U;
    permitted = path_valid = true; replace_during_authorization = false;
    operation_result = lookup_result = VFS_OK; lookup_calls=lookup_at=0;
}
'''
# Use kernel credentials types, which are the syscall's actual parameter type.
prefix = prefix.replace('#include <mg/credentials.h>', '#include <identity.h>')
body = block(source.index('case SYSCALL_PATH_MOVE:'))
code = prefix + function('syscall_vfs_error') + function('syscall_admin_same_node') + \
    function('syscall_admin_move') + 'static void dispatch_move(syscall_frame_t *frame)\n' + body + r'''
int main(void)
{
    for (unsigned mode = 0U; mode < 3U; mode++) {
        reset(); destination_mode = mode;
        syscall_frame_t frame = {.rdi = (u64)"source", .rsi = (u64)"destination"};
        dispatch_move(&frame);
        assert((i64)frame.rax == (mode == 2U ? MG_ERR_ALREADY_EXISTS : MG_OK));
        assert(moves == (mode == 2U ? 0U : 1U));
        reset(); destination_mode = mode;
        assert(syscall_admin_move(&process, "source", "destination") ==
            (mode == 2U ? MG_ERR_ALREADY_EXISTS : MG_OK));
        assert(moves == (mode == 2U ? 0U : 1U));
    }
    for(unsigned at=1;at<=4;at++) {
        reset(); lookup_result=VFS_ERR_IO; lookup_at=at;
        assert(syscall_admin_move(&process,"source","destination")==MG_ERR_IO && !moves);
        assert(authorizations==(at>2));
        if(at<=2) {
            reset(); lookup_result=VFS_ERR_IO;lookup_at=at;
            syscall_frame_t lookup_frame={.rdi=(u64)"source",.rsi=(u64)"destination"};
            dispatch_move(&lookup_frame);assert((i64)lookup_frame.rax==MG_ERR_IO && !moves);
        }
    }
    reset(); destination_mode = 1U; replace_during_authorization = true;
    assert(syscall_admin_move(&process, "source", "destination") == MG_ERR_ACCESS_DENIED && !moves);
    reset(); operation_result = VFS_ERR_UNCERTAIN;
    syscall_frame_t frame = {.rdi = (u64)"source", .rsi = (u64)"destination"};
    dispatch_move(&frame); assert((i64)frame.rax == MG_ERR_UNCERTAIN && moves == 1U);
    reset(); path_valid = false;
    dispatch_move(&frame); assert((i64)frame.rax == MG_ERR_BAD_ARGUMENT && !moves);
    reset(); permitted = false;
    dispatch_move(&frame); assert((i64)frame.rax == MG_ERR_ACCESS_DENIED && !moves);
    puts("Native move identity, exclusive destinations, authorization and uncertainty passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-path-move-') as directory:
    fixture, binary = Path(directory) / 'fixture.c', Path(directory) / 'fixture'
    fixture.write_text(code)
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'gcc', '-O1', '-std=gnu11',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(root / 'kernel/include'), '-I' + str(root / 'include'),
                    '-idirafter', str(root / 'libc/include'), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
