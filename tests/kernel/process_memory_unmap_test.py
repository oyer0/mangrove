#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise atomic process mapping removal from production process.c."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "kernel/src/process.c").read_text()


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
#include <pthread.h>
typedef uintptr_t uintptr_t_alias;
typedef size_t usize;
typedef int64_t i64;
typedef uint64_t u64;
typedef uint64_t phys_addr_t;
typedef struct page_table { unsigned unused; } page_table_t;
typedef pthread_mutex_t mutex_t;
#define PROCESS_STATE_ACTIVE 1
#define VMM_PAGE_SIZE 4096U
#define MG_ERR_BAD_ARGUMENT -1
#define MG_ERR_NO_MEMORY -2
#define MG_ERR_NOT_FOUND -3
#define MG_ERR_WOULD_BLOCK -4
#define MG_ERR_BUSY -5
#define MG_OK 0
typedef struct process_memory_mapping {
    uintptr_t address;
    usize page_count;
    struct process_memory_mapping *next;
} process_memory_mapping_t;
typedef struct process {
    int state;
    mutex_t memory_lock;
    process_memory_mapping_t *memory_mappings;
    page_table_t *address_space;
} process_t;
static unsigned frame_releases;
static bool range_busy;
static bool mutex_lock(mutex_t *lock){return pthread_mutex_lock(lock)==0;}
static void mutex_unlock(mutex_t *lock){assert(pthread_mutex_unlock(lock)==0);}
static void kfree(void *value){free(value);}
static bool vmm_unmap_user_range(page_table_t *space,void *address,
                                 usize count){
    assert(space&&address==(void*)0x400000U);
    if(range_busy)return false;
    frame_releases+=(unsigned)count;
    return true;
}
'''


def main():
    cc = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    code = FIXTURE + function("process_memory_unmap") + r'''
int main(void){
    process_t process={0};page_table_t space={0};
    process_memory_mapping_t *mapping=malloc(sizeof(*mapping));assert(mapping);
    *mapping=(process_memory_mapping_t){0x400000U,3,NULL};
    assert(pthread_mutex_init(&process.memory_lock,NULL)==0);
    process.state=PROCESS_STATE_ACTIVE;process.address_space=&space;
    process.memory_mappings=mapping;

    /* An active page lease makes VMM reject the whole range. The mapping
       descriptor remains linked and no physical frame is freed. */
    range_busy=true;
    assert(process_memory_unmap(&process,mapping->address)==MG_ERR_BUSY);
    assert(process.memory_mappings==mapping&&frame_releases==0U);

    /* Once all leases are gone, VMM removes the complete extent and releases
       its owned frames after the address-space flush, without a frame-sized
       temporary array in process memory. */
    range_busy=false;
    assert(process_memory_unmap(&process,mapping->address)==MG_OK);
    assert(process.memory_mappings==NULL&&frame_releases==3U);
    assert(pthread_mutex_destroy(&process.memory_lock)==0);
    puts("Process memory unmap: busy atomicity and bounded complete release PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="mangrove-process-unmap-") as directory:
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
