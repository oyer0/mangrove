#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production address-space teardown across a remote CPU switch."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "kernel/src/vmm.c").read_text()


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
#include <pthread.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef size_t usize;
typedef uint64_t phys_addr_t;
#define CPU_MAX_COUNT 2U
#define VMM_CPU_MASK_WORDS 1U
#define VMM_PAGE_SIZE 4096U
typedef struct page_table { u64 entries[512]; } page_table_t;
typedef pthread_mutex_t mutex_t;
typedef pthread_mutex_t spinlock_t;
typedef struct vmm_owned_frame {
    phys_addr_t physical;
    u32 user_pin_count;
    struct vmm_owned_frame *next;
} vmm_owned_frame_t;
typedef struct {
    page_table_t *root;
    vmm_owned_frame_t *leaves, *tables;
    pthread_mutex_t operation_lock;
    volatile u64 active_cpu_mask[VMM_CPU_MASK_WORDS];
    u64 active_user_leases;
    bool destroy_pending, shootdown_pending;
} vmm_address_space_metadata_t;
typedef struct cpu_local {
    u32 index;
    page_table_t *current_pml4;
} cpu_local_t;
static page_table_t *kernel_pml4;
static vmm_address_space_metadata_t metadata;
static cpu_local_t cpus[CPU_MAX_COUNT];
static __thread u32 executing_cpu;
static pthread_mutex_t vmm_metadata_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t vmm_kernel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t remote_start, remote_done;
static u32 cr3_writes, freed_frames;
#define active_pml4 (cpu_current()->current_pml4)
static cpu_local_t *cpu_current(void) { return &cpus[executing_cpu]; }
static u32 cpu_current_index(void) { return executing_cpu; }
static u64 spin_lock_irqsave(spinlock_t *lock)
{ assert(pthread_mutex_lock(lock)==0); return 0; }
static void spin_unlock_irqrestore(spinlock_t *lock,u64 flags)
{ (void)flags; assert(pthread_mutex_unlock(lock)==0); }
static bool mutex_lock(mutex_t *lock) { return pthread_mutex_lock(lock)==0; }
static void mutex_unlock(mutex_t *lock) { assert(pthread_mutex_unlock(lock)==0); }
static vmm_address_space_metadata_t *vmm_metadata_find(const page_table_t *root)
{ return metadata.root==root ? &metadata : NULL; }
static phys_addr_t vmm_table_phys(page_table_t *table)
{ return (phys_addr_t)(uintptr_t)table; }
static bool vmm_kernel_reaches_table(phys_addr_t physical)
{ (void)physical; return false; }
static void vmm_test_switch_cr3(phys_addr_t physical)
{ assert(physical==(phys_addr_t)(uintptr_t)kernel_pml4); cr3_writes++; }
static void pmm_free_frame(phys_addr_t physical)
{
    assert(!(metadata.active_cpu_mask[0] & 2U));
    assert(cr3_writes==1U);
    freed_frames++;
    free((void *)(uintptr_t)physical);
}
static void kfree(void *value) { free(value); }
static bool scheduler_sleep(u64 ticks)
{
    assert(executing_cpu==0U && ticks==1U);
    int result=pthread_barrier_wait(&remote_start);
    assert(result==0||result==PTHREAD_BARRIER_SERIAL_THREAD);
    result=pthread_barrier_wait(&remote_done);
    assert(result==0||result==PTHREAD_BARRIER_SERIAL_THREAD);
    return true;
}
static void panic(const char *message)
{ fprintf(stderr,"%s\n",message); abort(); }
'''


def main():
    cc = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    switch = function("vmm_switch_address_space")
    switch = re.sub(
        r'__asm__ volatile\("mov %0, %%cr3"\s*::\s*"r"\s*\(vmm_table_phys\(pml4\)\)\s*:\s*"memory"\);',
        "vmm_test_switch_cr3(vmm_table_phys(pml4));", switch)
    code = FIXTURE + function("vmm_cpu_mask_empty") + switch + \
        function("vmm_destroy_address_space")
    code += r'''
static void *remote_cpu(void *unused)
{
    (void)unused;
    executing_cpu=1U;
    int result=pthread_barrier_wait(&remote_start);
    assert(result==0||result==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(vmm_switch_address_space(kernel_pml4));
    /* destroy_pending forbids this CPU from reacquiring the address space
       after its active-root reference has been dropped. */
    assert(!vmm_switch_address_space(metadata.root));
    result=pthread_barrier_wait(&remote_done);
    assert(result==0||result==PTHREAD_BARRIER_SERIAL_THREAD);
    return NULL;
}
int main(void)
{
    page_table_t *root=aligned_alloc(VMM_PAGE_SIZE,sizeof(*root));
    page_table_t kernel={0};
    assert(root); memset(root,0,sizeof(*root));
    kernel_pml4=&kernel;
    metadata.root=root;
    metadata.active_cpu_mask[0]=2U;
    assert(pthread_mutex_init(&metadata.operation_lock,NULL)==0);
    cpus[0]=(cpu_local_t){.index=0,.current_pml4=&kernel};
    cpus[1]=(cpu_local_t){.index=1,.current_pml4=root};
    vmm_owned_frame_t *leaf=malloc(sizeof(*leaf));assert(leaf);
    leaf->physical=(phys_addr_t)(uintptr_t)aligned_alloc(VMM_PAGE_SIZE,
                                                        VMM_PAGE_SIZE);
    assert(leaf->physical);leaf->user_pin_count=0;leaf->next=NULL;
    metadata.leaves=leaf;
    assert(pthread_barrier_init(&remote_start,NULL,2)==0);
    assert(pthread_barrier_init(&remote_done,NULL,2)==0);
    pthread_t remote;
    assert(pthread_create(&remote,NULL,remote_cpu,NULL)==0);

    executing_cpu=0U;
    vmm_destroy_address_space(root);
    assert(pthread_join(remote,NULL)==0);
    assert(cr3_writes==1U && freed_frames==2U);
    assert(metadata.root==NULL && metadata.leaves==NULL &&
           !metadata.active_cpu_mask[0] && !metadata.destroy_pending);
    assert(pthread_mutex_destroy(&metadata.operation_lock)==0);
    assert(pthread_barrier_destroy(&remote_start)==0 &&
           pthread_barrier_destroy(&remote_done)==0);
    puts("VMM address-space teardown: remote CR3 switch precedes root/frame reclamation and destroy-pending blocks reacquisition PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="mangrove-vmm-lifetime-") as temp:
        source = Path(temp) / "fixture.c"
        binary = Path(temp) / "fixture"
        source.write_text(code)
        subprocess.run([cc, "-std=gnu17", "-Wall", "-Wextra", "-Werror",
                        "-pthread", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", str(source), "-o",
                        str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


if __name__ == "__main__":
    main()
