#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded native pathname copying at the syscall trust boundary."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "kernel/src/syscall.c").read_text()
match = re.search(r"^static int syscall_copy_path\(", source, re.M)
assert match, "syscall_copy_path not found"
brace = source.index("{", match.end())
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[match.start():end]
vfs_source = (root / "kernel/src/vfs.c").read_text()

def extract_function(text, name):
    found = re.search(r"^(?:static )?[^\n;{}]*\b" + name + r"\s*\(", text, re.M)
    assert found, name
    brace = text.index("{", found.end())
    depth, finish = 1, brace + 1
    while depth:
        depth += (text[finish] == "{") - (text[finish] == "}")
        finish += 1
    return text[found.start():finish]

vfs_functions = extract_function(vfs_source, "vfs_path_cstr_length") + "\n" + \
                extract_function(vfs_source, "vfs_path_validate_n")
syscall_copy_byte = extract_function(source, "syscall_copy_from_user_byte")
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>
#include <mg/filesystem.h>
#include <mg/utf8.h>
#include <vfs.h>
static uintptr_t readable_start, readable_end;
static bool fail_alloc;
typedef struct { void *address_space; } process_t;
typedef struct { process_t *process; void *syscall_user_access_context; }
    kernel_thread_t;
typedef struct {
    void *address_space; uintptr_t first_page; usize page_count; bool active;
} vmm_user_range_lease_t;
static process_t test_process;
static kernel_thread_t test_thread={.process=&test_process};
static unsigned live_leases, lease_acquisitions, lease_releases;
static kernel_thread_t *thread_current(void) { return &test_thread; }
static bool vmm_user_range_lease(void *address_space,const void *pointer,
                                 usize length,bool writable,
                                 vmm_user_range_lease_t *lease) {
    (void)writable;
    uintptr_t start=(uintptr_t)pointer;
    if (!length || start < readable_start || start > readable_end ||
        length > readable_end-start) return false;
    lease->address_space=address_space;
    lease->first_page=start & ~(uintptr_t)4095U;
    lease->page_count=1;
    lease->active=true;
    live_leases++; lease_acquisitions++;
    return true;
}
static void vmm_user_range_release(vmm_user_range_lease_t *lease) {
    assert(lease && lease->active && live_leases);
    lease->active=false; live_leases--; lease_releases++;
}
static void *kmalloc(usize n) { return fail_alloc ? NULL : malloc(n); }
static void kfree(void *p) { free(p); }
'''+syscall_copy_byte+'\n'+function+r'''
int main(void) {
    char *source = malloc(MG_PATH_MAX_BYTES + 2U), *copied = NULL;
    assert(source); usize length=99;
    memset(source, 'a', MG_PATH_MAX_BYTES + 2U); source[0]='x'; source[1]=0;
    readable_start=(uintptr_t)source; readable_end=readable_start+MG_PATH_MAX_BYTES+2U;
    assert(syscall_copy_path(source,&copied,&length) == VFS_OK);
    assert(length==1 && !strcmp(copied,"x"));
    assert(!live_leases && lease_acquisitions==lease_releases);
    kfree(copied); copied=NULL;
    memset(source,'b',MG_PATH_MAX_BYTES); source[MG_PATH_MAX_BYTES]=0; length=0;
    assert(syscall_copy_path(source,&copied,&length) == VFS_OK);
    assert(length==MG_PATH_MAX_BYTES && copied[MG_PATH_MAX_BYTES]==0);
    assert(!live_leases && lease_acquisitions==lease_releases);
    kfree(copied); copied=NULL;
    memset(source,'c',MG_PATH_MAX_BYTES + 1U); source[MG_PATH_MAX_BYTES + 1U]=0; length=73;
    assert(syscall_copy_path(source,&copied,&length) == VFS_ERR_NAME_TOO_LONG);
    assert(!copied && length==0); /* Overlong input is rejected, never truncated. */
    assert(!live_leases && lease_acquisitions==lease_releases);
    source[0]=0;
    assert(syscall_copy_path(source,&copied,&length) == VFS_ERR_INVALID_PARAM);
    assert(syscall_copy_path(NULL,&copied,&length) == VFS_ERR_INVALID_PARAM);
    assert(syscall_copy_path(source,NULL,&length) == VFS_ERR_INVALID_PARAM);
    assert(syscall_copy_path(source,&copied,NULL) == VFS_ERR_INVALID_PARAM);
    assert(syscall_copy_path((const char *)(uintptr_t)(~(uintptr_t)0-1U),
                             &copied,&length) == VFS_ERR_INVALID_PARAM);
    assert(!live_leases && lease_acquisitions==lease_releases);
    source[0]='x'; source[1]=0; fail_alloc=true;
    assert(syscall_copy_path(source,&copied,&length) == VFS_ERR_NO_MEM && !copied);
    assert(!live_leases && lease_acquisitions==lease_releases);
    fail_alloc=false;
    puts("dynamic pathname copy: 65535-byte admission, no truncation, pointer and allocation failures PASS");
    free(source);

    char *path = malloc(MG_PATH_MAX_BYTES + 2U); usize path_length=0;
    assert(path); memset(path, 'a', MG_PATH_MAX_BYTES + 1U);
    path[MG_PATH_MAX_BYTES]=0;
    assert(vfs_path_cstr_length(path,&path_length)==VFS_OK &&
           path_length==MG_PATH_MAX_BYTES);
    path[MG_PATH_MAX_BYTES]='b'; path[MG_PATH_MAX_BYTES+1U]=0;
    assert(vfs_path_cstr_length(path,&path_length)==VFS_ERR_NAME_TOO_LONG);
    path[0]='/'; path[1]=(char)0xff;
    assert(vfs_path_validate_n(path,2U)==VFS_ERR_INVALID_PARAM);
    path[1]='a'; path[2]=0;
    assert(vfs_path_validate_n(path,3U)==VFS_ERR_INVALID_PARAM);
    path[1]='\xc3'; path[2]='\xa9';
    assert(vfs_path_validate_n(path,3U)==VFS_OK);
    free(path);
}
'''
fixture = fixture.replace("int main(void)", vfs_functions + "\nint main(void)")
with tempfile.TemporaryDirectory(prefix="mangrove-path-copy-") as work:
    work = Path(work)
    c_file, binary = work / "fixture.c", work / "fixture"
    c_file.write_text(fixture)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else "gcc", "-std=gnu11",
        "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I"+str(root/"kernel/include"), "-I"+str(root/"include"),
        "-I"+str(root/"libc/include"),
        str(c_file), str(root/"libc/src/utf8.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
