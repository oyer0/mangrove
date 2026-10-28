#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise syscall buffer lease tracking from production syscall.c."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "kernel/src/syscall.c").read_text()


def function(name):
    pattern = re.compile(r"^(?:static )?[A-Za-z_][^\n;{}]*\b" +
                         re.escape(name) + r"\s*\(", re.M)
    for match in pattern.finditer(SOURCE):
        brace = SOURCE.index("{", match.end())
        if ";" in SOURCE[match.end():brace]:
            continue
        depth = 1
        end = brace + 1
        while depth:
            depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
            end += 1
        return SOURCE[match.start():end] + "\n"
    raise AssertionError(name)


FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t usize;
typedef uint64_t u64;
typedef struct page_table { unsigned unused; } page_table_t;
#define VMM_PAGE_SIZE 4096U
#define VMM_USER_ADDRESS_LIMIT 0x800000000000ULL
typedef struct {
    page_table_t *address_space;
    uintptr_t first_page;
    usize page_count;
    bool active;
} vmm_user_range_lease_t;
typedef struct {
    bool writable;
    vmm_user_range_lease_t lease;
} syscall_user_lease_entry_t;
typedef struct {
    syscall_user_lease_entry_t *leases;
    usize count;
    usize capacity;
    bool allocation_failed;
} syscall_user_access_context_t;
typedef struct process { page_table_t *address_space; } process_t;
typedef struct kernel_thread {
    process_t *process;
    void *syscall_user_access_context;
} kernel_thread_t;
static kernel_thread_t test_thread;
static kernel_thread_t *thread_current(void) { return &test_thread; }
static struct { uintptr_t page; bool mapped; bool writable; unsigned pins; }
    pages[4];
static long allocation_countdown = -1;
static void *kmalloc(usize size) {
    if (allocation_countdown == 0) { allocation_countdown = -1; return NULL; }
    if (allocation_countdown > 0) allocation_countdown--;
    return malloc(size);
}
static void kfree(void *value) { free(value); }
static bool vmm_user_range_lease(page_table_t *space, const void *buffer,
                                 usize length, bool writable,
                                 vmm_user_range_lease_t *out) {
    uintptr_t start=(uintptr_t)buffer;
    uintptr_t end=start+length;
    usize first=0,last=0;
    for (usize i=0;i<4;i++) if(pages[i].page==(start&~(uintptr_t)4095U))first=i;
    for (usize i=0;i<4;i++) if(pages[i].page==((end-1U)&~(uintptr_t)4095U))last=i;
    if (!length || last<first || !space) return false;
    for(usize i=first;i<=last;i++) if(!pages[i].mapped||
        (writable&&!pages[i].writable)) return false;
    for(usize i=first;i<=last;i++) pages[i].pins++;
    *out=(vmm_user_range_lease_t){space,start&~(uintptr_t)4095U,
                                  last-first+1U,true};
    return true;
}
static void vmm_user_range_release(vmm_user_range_lease_t *lease) {
    assert(lease&&lease->active);
    for(usize i=0;i<lease->page_count;i++) {
        uintptr_t page=lease->first_page+i*VMM_PAGE_SIZE;
        bool found=false;
        for(usize j=0;j<4;j++) if(pages[j].page==page) {
            assert(pages[j].pins);pages[j].pins--;found=true;break;
        }
        assert(found);
    }
    lease->active=false;
}
'''


def main():
    cc = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    code = FIXTURE + "\n".join(function(name) for name in (
        "syscall_user_buffer_accessible", "syscall_user_buffer_valid",
        "syscall_user_buffer_writable", "syscall_user_lease_release_all"))
    code += r'''
int main(void) {
    page_table_t space={0}; process_t process={&space};
    test_thread.process=&process;
    syscall_user_access_context_t context={0};
    test_thread.syscall_user_access_context=&context;
    for(usize i=0;i<4;i++) pages[i]=(typeof(pages[i])){
        .page=0x400000U+i*VMM_PAGE_SIZE,.mapped=true,.writable=true};

    /* A single range lease protects a cross-page buffer without allocating
       one bookkeeping record per page. Repeated checks reuse coverage, and
       a writable check adds a stronger lease when needed. */
    assert(syscall_user_buffer_valid((void*)0x400ff0U,32));
    assert(context.count==1&&pages[0].pins==1&&pages[1].pins==1);
    assert(syscall_user_buffer_valid((void*)0x400ff0U,32));
    assert(context.count==1&&pages[0].pins==1&&pages[1].pins==1);
    assert(syscall_user_buffer_writable((void*)0x400ff0U,32));
    assert(context.count==2&&pages[0].pins==2&&pages[1].pins==2);
    assert(syscall_user_buffer_valid((void*)0x401000U,1));
    assert(context.count==2&&pages[1].pins==2);
    syscall_user_lease_release_all(&context);
    assert(!context.count&&!context.leases&&!pages[0].pins&&!pages[1].pins);

    /* If a later, separate buffer is invalid, earlier syscall leases remain
       owned by dispatch cleanup and are released before returning to userspace. */
    assert(syscall_user_buffer_valid((void*)0x400000U,16));
    pages[1].mapped=false;
    assert(!syscall_user_buffer_valid((void*)0x401000U,1));
    assert(context.count==1&&pages[0].pins==1);
    syscall_user_lease_release_all(&context);
    assert(!pages[0].pins);
    pages[1].mapped=true;

    /* Allocation and permission failures never publish a successful access
       and always permit cleanup of previously pinned pages. */
    allocation_countdown=0;
    assert(!syscall_user_buffer_valid((void*)0x400000U,1));
    assert(context.allocation_failed&&!context.count&&!pages[0].pins);
    context.allocation_failed=false;
    pages[1].writable=false;
    assert(syscall_user_buffer_valid((void*)0x400000U,1));
    assert(!syscall_user_buffer_writable((void*)0x400ff0U,32));
    assert(context.count==1&&pages[0].pins==1);
    syscall_user_lease_release_all(&context);
    assert(!pages[0].pins);
    pages[1].writable=true;

    assert(syscall_user_buffer_valid(NULL,0));
    assert(!syscall_user_buffer_valid((void*)(uintptr_t)(VMM_USER_ADDRESS_LIMIT-1U),2));
    assert(!syscall_user_buffer_valid((void*)(uintptr_t)UINTPTR_MAX-8U,32));
    assert(!context.count&&!context.leases);
    puts("Syscall user-memory leases: range ownership, reuse, partial failure and cleanup PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="mangrove-syscall-memory-") as directory:
        source = Path(directory) / "fixture.c"
        binary = Path(directory) / "fixture"
        source.write_text(code)
        subprocess.run([cc, "-std=gnu17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        str(source), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


if __name__ == "__main__":
    main()
