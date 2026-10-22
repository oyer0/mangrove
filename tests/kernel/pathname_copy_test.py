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
match = re.search(r"^static bool syscall_copy_path\(", source, re.M)
assert match, "syscall_copy_path not found"
brace = source.index("{", match.end())
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[match.start():end]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <types.h>
#include <mg/filesystem.h>
static uintptr_t readable_start, readable_end;
static bool vmm_user_range_valid(const void *pointer, usize length) {
    uintptr_t start=(uintptr_t)pointer;
    if (!length || start < readable_start || start > readable_end) return false;
    return length <= readable_end - start;
}
'''+function+r'''
int main(void) {
    char source[258], copied[256]; usize length=99;
    memset(source, 'a', sizeof(source)); source[0]='x'; source[1]=0;
    readable_start=(uintptr_t)source; readable_end=readable_start+sizeof(source);
    assert(syscall_copy_path(source,copied,sizeof(copied),&length));
    assert(length==1 && !strcmp(copied,"x"));
    memset(source,'b',255); source[255]=0; length=0;
    assert(syscall_copy_path(source,copied,sizeof(copied),&length));
    assert(length==MG_PATH_MAX_BYTES && copied[255]==0);
    memset(source,'c',256); length=73;
    assert(!syscall_copy_path(source,copied,sizeof(copied),&length));
    assert(length==73); /* An overlong string is rejected, never truncated. */
    source[0]=0;
    assert(!syscall_copy_path(source,copied,sizeof(copied),&length));
    assert(!syscall_copy_path(NULL,copied,sizeof(copied),&length));
    assert(!syscall_copy_path(source,copied,1,&length));
    assert(!syscall_copy_path(source,copied,257,&length));
    assert(!syscall_copy_path((const char *)(uintptr_t)(~(uintptr_t)0-1U),
                              copied,sizeof(copied),&length));
    puts("bounded pathname copy, exact limit, malformed ranges and no-truncation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix="mangrove-path-copy-") as work:
    work = Path(work)
    c_file, binary = work / "fixture.c", work / "fixture"
    c_file.write_text(fixture)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else "gcc", "-std=gnu11",
        "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I"+str(root/"include"), "-I"+str(root/"libc/include"),
        str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
