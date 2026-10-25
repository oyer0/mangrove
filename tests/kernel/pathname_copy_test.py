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
static bool vmm_user_range_valid(const void *pointer, usize length) {
    uintptr_t start=(uintptr_t)pointer;
    if (!length || start < readable_start || start > readable_end) return false;
    return length <= readable_end - start;
}
static void *kmalloc(usize n) { return fail_alloc ? NULL : malloc(n); }
static void kfree(void *p) { free(p); }
'''+function+r'''
int main(void) {
    char *source = malloc(MG_PATH_MAX_BYTES + 2U), *copied = NULL;
    assert(source); usize length=99;
    memset(source, 'a', MG_PATH_MAX_BYTES + 2U); source[0]='x'; source[1]=0;
    readable_start=(uintptr_t)source; readable_end=readable_start+MG_PATH_MAX_BYTES+2U;
    assert(syscall_copy_path(source,&copied,&length) == VFS_OK);
    assert(length==1 && !strcmp(copied,"x"));
    kfree(copied); copied=NULL;
    memset(source,'b',MG_PATH_MAX_BYTES); source[MG_PATH_MAX_BYTES]=0; length=0;
    assert(syscall_copy_path(source,&copied,&length) == VFS_OK);
    assert(length==MG_PATH_MAX_BYTES && copied[MG_PATH_MAX_BYTES]==0);
    kfree(copied); copied=NULL;
    memset(source,'c',MG_PATH_MAX_BYTES + 1U); source[MG_PATH_MAX_BYTES + 1U]=0; length=73;
    assert(syscall_copy_path(source,&copied,&length) == VFS_ERR_NAME_TOO_LONG);
    assert(!copied && length==0); /* Overlong input is rejected, never truncated. */
    source[0]=0;
    assert(syscall_copy_path(source,&copied,&length) == VFS_ERR_INVALID_PARAM);
    assert(syscall_copy_path(NULL,&copied,&length) == VFS_ERR_INVALID_PARAM);
    assert(syscall_copy_path(source,NULL,&length) == VFS_ERR_INVALID_PARAM);
    assert(syscall_copy_path(source,&copied,NULL) == VFS_ERR_INVALID_PARAM);
    assert(syscall_copy_path((const char *)(uintptr_t)(~(uintptr_t)0-1U),
                             &copied,&length) == VFS_ERR_INVALID_PARAM);
    source[0]='x'; source[1]=0; fail_alloc=true;
    assert(syscall_copy_path(source,&copied,&length) == VFS_ERR_NO_MEM && !copied);
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
