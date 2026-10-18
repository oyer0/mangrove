#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production launch-path functions with host memory/filesystem stubs."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else 'cc'


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(', source, re.M)
    assert match, name
    brace = source.index('{', match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


COMMON = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
typedef unsigned long long u64; typedef unsigned int u32; typedef unsigned char u8;
typedef long long i64; typedef size_t usize; typedef long isize;
#define MG_FILE_LINES_MAX 4096U
#define MG_FILE_LINE_CAPACITY 512U
#define MG_FILE_LINES_RESET 1U
#define VFS_OK 0
#define VFS_ERR_INVALID_PARAM -1
#define VFS_ERR_IO -3
#define VFS_ERR_NO_MEM -4
#define VFS_ERR_ACCESS_DENIED -8
#define VFS_ERR_DEVICE_GONE -11
#define VFS_ERR_UNSUPPORTED -6
#define VFS_OPEN_RDWR 3U
'''


def run(name, source):
    with tempfile.TemporaryDirectory(prefix='mangrove-launch-') as directory:
        path = Path(directory) / (name + '.c')
        binary = path.with_suffix('')
        path.write_text(COMMON + source)
        subprocess.run([CC, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


vmm = r'''
#define VMM_PAGE_SIZE 4096ULL
#define VMM_USER_ADDRESS_LIMIT 0x800000000000ULL
#define VMM_CPU_MASK_WORDS 1
#define PTE_USER 4ULL
#define PTE_READWRITE 2ULL
#define PTE_PRESENT 1ULL
#define PTE_HUGE 128ULL
#define PTE_FRAME_MASK 0x000ffffffffff000ULL
typedef u64 phys_addr_t;
typedef struct {u64 entries[512];} page_table_t;
typedef struct {bool held;} mutex_t;
typedef int spinlock_t;
typedef struct vmm_owned_frame {phys_addr_t physical;struct vmm_owned_frame*next;} vmm_owned_frame_t;
typedef struct {page_table_t *root;vmm_owned_frame_t *tables,*leaves;mutex_t operation_lock;bool destroy_pending,shootdown_pending;} vmm_address_space_metadata_t;
static page_table_t *kernel_pml4,*active_pml4;static spinlock_t vmm_metadata_lock,vmm_kernel_lock;
static vmm_address_space_metadata_t meta;
static unsigned table_allocations,table_frees;static int fail_after=-1;
static bool mutex_lock(mutex_t*m){assert(!m->held);m->held=true;return true;}
static void mutex_unlock(mutex_t*m){assert(m->held);m->held=false;}
static u64 spin_lock_irqsave(spinlock_t*l){assert(!*l);*l=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t*l,u64 flags){(void)flags;assert(*l);*l=0;}
static bool phys_map_contains(u64 p){return p!=0;}
static page_table_t *vmm_alloc_table(void){if(fail_after==0)return NULL;if(fail_after>0)fail_after--;table_allocations++;page_table_t*p=aligned_alloc(4096,4096);assert(p);memset(p,0,4096);return p;}
static void pmm_free_frame(u64 p){table_frees++;free((void*)(uintptr_t)p);}
static phys_addr_t vmm_table_phys(page_table_t*t){return (u64)(uintptr_t)t;}
static page_table_t *vmm_table_from_phys(u64 p){return (page_table_t*)(uintptr_t)(p&PTE_FRAME_MASK);}
static void *kmalloc(usize n){assert(!vmm_metadata_lock&&!vmm_kernel_lock);return malloc(n);}
static void kfree(void*p){free(p);}
static vmm_address_space_metadata_t*vmm_metadata_find(const page_table_t*p){return p==meta.root?&meta:NULL;}
static bool vmm_owned_contains(const vmm_owned_frame_t*n,u64 p){for(;n;n=n->next)if(n->physical==p)return true;return false;}
static void vmm_owned_insert(vmm_owned_frame_t**list,vmm_owned_frame_t*n,u64 p){n->physical=p;n->next=*list;*list=n;}
static bool vmm_address_space_validate_locked(page_table_t*p){return p==meta.root;}
static void vmm_capture_active_targets_locked(vmm_address_space_metadata_t*m,u64*out){(void)m;out[0]=0;}
static void vmm_tlb_shootdown(u64*targets,page_table_t*p){(void)targets;(void)p;assert(!vmm_metadata_lock&&!vmm_kernel_lock);}
static void panic(const char*s){fprintf(stderr,"%s\n",s);abort();}
static void invalidate_page(void *p){(void)p;}
'''
for name in ['vmm_user_child_reserved','vmm_user_path_available',
             'vmm_map_user_page','vmm_user_range_accessible',
             'vmm_user_range_valid','vmm_user_range_writable']:
    code = function('kernel/src/vmm.c', name)
    code = re.sub(r'__asm__ volatile\("invlpg.*?;', 'invalidate_page(virtual_addr);', code, flags=re.S)
    vmm += code
vmm += r'''
static u64 map(u64 address){u64 frame=(u64)(uintptr_t)aligned_alloc(4096,4096);assert(frame);assert(vmm_map_user_page(meta.root,(void*)(uintptr_t)address,frame,PTE_USER|PTE_READWRITE));assert(!meta.operation_lock.held);return frame;}
int main(void){
 kernel_pml4=aligned_alloc(4096,4096);memset(kernel_pml4,0,4096);meta.root=aligned_alloc(4096,4096);memset(meta.root,0,4096);
 active_pml4=meta.root;
 u64 first=map(0x400000);assert(table_allocations==3&&table_frees==0);map(0x401000);assert(table_allocations==3);map(0x600000);assert(table_allocations==4);map(0x40000000);assert(table_allocations==6);map(1ULL<<39);assert(table_allocations==9);
 assert(vmm_user_range_writable((void*)0x400000,4096));
 assert(!vmm_user_range_writable((void*)(uintptr_t)(0x100000000000000ULL+0x400000ULL),4));
 assert(!vmm_user_range_writable((void*)(uintptr_t)(VMM_USER_ADDRESS_LIMIT-2),4));
 assert(!vmm_user_range_writable((void*)0x400000,8192+1));
 u64 read_only=(u64)(uintptr_t)aligned_alloc(4096,4096);assert(read_only);
 assert(vmm_map_user_page(meta.root,(void*)0x402000,read_only,PTE_USER));
 assert(vmm_user_range_valid((void*)0x402000,4));
 assert(!vmm_user_range_writable((void*)0x402000,4));
 assert(!vmm_user_range_writable((void*)0x401fff,2));
 assert(vmm_user_range_writable((void*)0x401fff,1));
 assert(!vmm_map_user_page(meta.root,(void*)0x400000,first,PTE_USER));assert(table_allocations==9&&!meta.operation_lock.held);
 assert(!vmm_map_user_page(meta.root,(void*)0x402000,first,PTE_USER));assert(!meta.operation_lock.held);
 fail_after=1;u64 frame=(u64)(uintptr_t)aligned_alloc(4096,4096);assert(!vmm_map_user_page(meta.root,(void*)(2ULL<<39),frame,PTE_USER));assert(table_allocations==10&&table_frees==1&&!meta.operation_lock.held);free((void*)(uintptr_t)frame);fail_after=-1;
 meta.root->entries[3]=vmm_table_phys(kernel_pml4)|PTE_PRESENT;frame=(u64)(uintptr_t)aligned_alloc(4096,4096);assert(!vmm_map_user_page(meta.root,(void*)(3ULL<<39),frame,PTE_USER));free((void*)(uintptr_t)frame);assert(table_allocations==10);
 for(unsigned i=0;i<512;i++)assert(kernel_pml4->entries[i]==0);
 while(meta.leaves){vmm_owned_frame_t*n=meta.leaves;meta.leaves=n->next;free((void*)(uintptr_t)n->physical);free(n);}
 while(meta.tables){vmm_owned_frame_t*n=meta.tables;meta.tables=n->next;free((void*)(uintptr_t)n->physical);free(n);}free(meta.root);free(kernel_pml4);
 puts("page tables: allocations 3/0/1/2/3, duplicate/unowned rejection, failure cleanup, kernel isolation passed");
}
'''


run('vmm', vmm)


kernel_map = r'''
#define VMM_USER_ADDRESS_LIMIT 0x0000800000000000ULL
#define VMM_DIRECT_MAP_PML4_FIRST ((0xffff800000000000ULL >> 39) & 0x1ffU)
#define VMM_DIRECT_MAP_PML4_LAST (((0xffff800000000000ULL + 0x0000400000000000ULL - 1U) >> 39) & 0x1ffU)
#define VMM_HEAP_PML4_INDEX ((0xffffc00000000000ULL >> 39) & 0x1ffU)
#define VMM_IOREMAP_PML4_INDEX ((0xffffc08000000000ULL >> 39) & 0x1ffU)
#define VMM_IMAGE_PML4_INDEX ((0xffffffff80000000ULL >> 39) & 0x1ffU)
#define PTE_PRESENT 1ULL
#define PTE_READWRITE 2ULL
#define PTE_USER 4ULL
#define PTE_HUGE 128ULL
#define PTE_FRAME_MASK 0x000ffffffffff000ULL
#define PTE_NX (1ULL << 63)
typedef u64 phys_addr_t;
typedef struct { u64 entries[512]; } page_table_t;
static page_table_t *kernel_pml4, *active_pml4;
static unsigned table_attempts, table_allocations, table_frees;
static int fail_after = -1;
static page_table_t *allocated_tables[32];
static bool table_live[32];
static unsigned table_slots;
static bool phys_map_contains(u64 physical) { return physical != 0; }
static phys_addr_t vmm_table_phys(const page_table_t *table) {
    return (phys_addr_t)(uintptr_t)table;
}
static page_table_t *vmm_table_from_phys(phys_addr_t physical) {
    return (page_table_t *)(uintptr_t)(physical & PTE_FRAME_MASK);
}
static page_table_t *vmm_alloc_table(void) {
    table_attempts++;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    page_table_t *table = aligned_alloc(4096U, 4096U);
    assert(table && table_slots < 32U);
    memset(table, 0, 4096U);
    allocated_tables[table_slots] = table;
    table_live[table_slots++] = true;
    table_allocations++;
    return table;
}
static void pmm_free_frame(phys_addr_t physical) {
    page_table_t *table = (page_table_t *)(uintptr_t)physical;
    for (unsigned i = 0; i < table_slots; i++) {
        if (allocated_tables[i] == table && table_live[i]) {
            table_live[i] = false;
            table_frees++;
            free(table);
            return;
        }
    }
    assert(!"VMM released a table frame it did not allocate");
}
static bool vmm_kernel_shared_pml4_index(u32 index) {
    return (index >= VMM_DIRECT_MAP_PML4_FIRST &&
            index <= VMM_DIRECT_MAP_PML4_LAST) ||
           index == VMM_HEAP_PML4_INDEX ||
           index == VMM_IOREMAP_PML4_INDEX ||
           index == VMM_IMAGE_PML4_INDEX;
}
'''
kernel_map += function('kernel/src/vmm.c', 'vmm_map_kernel_one_internal')
kernel_map += r'''
static u64 base = 0xffffc00000000000ULL;
static void reset_root(void) {
    kernel_pml4 = aligned_alloc(4096U, 4096U);
    assert(kernel_pml4);
    memset(kernel_pml4, 0, 4096U);
    active_pml4 = NULL;
    table_attempts = table_allocations = table_frees = table_slots = 0;
    memset(allocated_tables, 0, sizeof(allocated_tables));
    memset(table_live, 0, sizeof(table_live));
    fail_after = -1;
}
static void assert_root_slot_empty(u32 slot) {
    assert(kernel_pml4->entries[slot] == 0);
}
static void release_tables_and_root(void) {
    for (unsigned i = 0; i < table_slots; i++) {
        if (table_live[i]) pmm_free_frame(vmm_table_phys(allocated_tables[i]));
    }
    free(kernel_pml4);
    kernel_pml4 = NULL;
}
int main(void) {
    reset_root();
    fail_after = 0;
    assert(!vmm_map_kernel_one_internal(kernel_pml4, (void *)(uintptr_t)base,
                                        0x12345000U,
                                        PTE_PRESENT | PTE_READWRITE, false));
    assert(table_attempts == 1U && table_allocations == 0U &&
           table_frees == 0U);
    assert_root_slot_empty(VMM_HEAP_PML4_INDEX);
    release_tables_and_root();

    for (int successful_allocations = 1; successful_allocations <= 2;
         successful_allocations++) {
        reset_root();
        fail_after = successful_allocations;
        assert(!vmm_map_kernel_one_internal(kernel_pml4,
                    (void *)(uintptr_t)base, 0x12345000U,
                    PTE_PRESENT | PTE_READWRITE, false));
        assert(table_attempts == (unsigned)successful_allocations + 1U);
        assert(table_allocations == (unsigned)successful_allocations &&
               table_frees == table_allocations);
        assert_root_slot_empty(VMM_HEAP_PML4_INDEX);
        release_tables_and_root();
    }

    reset_root();
    assert(vmm_map_kernel_one_internal(kernel_pml4, (void *)(uintptr_t)base,
                                       0x12345000U,
                                       PTE_PRESENT | PTE_READWRITE, false));
    assert(table_allocations == 3U && table_frees == 0U);
    u32 root_index = VMM_HEAP_PML4_INDEX;
    page_table_t *pdpt = vmm_table_from_phys(kernel_pml4->entries[root_index]);
    page_table_t *pd = vmm_table_from_phys(pdpt->entries[(base >> 30) & 0x1ffU]);
    page_table_t *pt = vmm_table_from_phys(pd->entries[(base >> 21) & 0x1ffU]);
    assert(pt->entries[(base >> 12) & 0x1ffU] ==
           (0x12345000U | PTE_PRESENT | PTE_READWRITE));

    /* An allocation failure below an existing PDPT must leave that PDPT's
     * target entry unchanged and return every detached table frame. */
    u64 next_gib = base + (1ULL << 30);
    fail_after = 1;
    unsigned allocations_before = table_allocations;
    unsigned frees_before = table_frees;
    assert(!vmm_map_kernel_one_internal(kernel_pml4,
                (void *)(uintptr_t)next_gib, 0x22345000U,
                PTE_PRESENT | PTE_READWRITE, false));
    assert(table_allocations == allocations_before + 1U &&
           table_frees == frees_before + 1U);
    assert(pdpt->entries[(next_gib >> 30) & 0x1ffU] == 0);

    /* Allocation at the leaf-table level is also failure-atomic. */
    u64 next_2m = base + (1ULL << 21);
    fail_after = 0;
    assert(!vmm_map_kernel_one_internal(kernel_pml4,
                (void *)(uintptr_t)next_2m, 0x32345000U,
                PTE_PRESENT | PTE_READWRITE, false));
    assert(pd->entries[(next_2m >> 21) & 0x1ffU] == 0);

    /* A new PML4 subtree is built fully before its root entry is installed. */
    u64 next_pml4 = base + (1ULL << 39);
    fail_after = 2;
    allocations_before = table_allocations;
    frees_before = table_frees;
    assert(!vmm_map_kernel_one_internal(kernel_pml4,
                (void *)(uintptr_t)next_pml4, 0x42345000U,
                PTE_PRESENT | PTE_READWRITE, false));
    assert(table_allocations == allocations_before + 2U &&
           table_frees == frees_before + 2U);
    assert_root_slot_empty(root_index + 1U);

    fail_after = -1;
    assert(vmm_map_kernel_one_internal(kernel_pml4,
                (void *)(uintptr_t)(base + 4096U), 0x52345000U,
                PTE_PRESENT | PTE_READWRITE, false));
    assert(table_allocations - table_frees == 3U);
    release_tables_and_root();
    puts("kernel mappings: detached page-table construction and allocation-failure ownership passed");
}
'''
run('vmm-kernel-map', kernel_map)


bootstrap_guard = r'''
typedef struct { bool bsp, online; } cpu_local_t;
static cpu_local_t cpu, extra_cpu;
static u32 online_cpus, discovered_cpus, map_calls;
static bool interrupts_enabled, map_result = true;
static cpu_local_t *cpu_current(void) { return &cpu; }
static u32 cpu_online_count(void) { return online_cpus; }
static u32 cpu_count(void) { return discovered_cpus; }
static cpu_local_t *cpu_by_index(u32 index) {
    if (index == 0U) return &cpu;
    if (index == 1U) return &extra_cpu;
    return NULL;
}
static bool vmm_interrupts_enabled(void) { return interrupts_enabled; }
typedef int page_table_t;
typedef u64 phys_addr_t;
static bool vmm_map(page_table_t *pml4, void *address, phys_addr_t physical,
                    u64 flags) {
    (void)pml4; (void)address; (void)physical; (void)flags;
    map_calls++;
    return map_result;
}
'''
bootstrap_guard += function('kernel/src/vmm.c', 'vmm_map_kernel_bootstrap')
bootstrap_guard += r'''
int main(void) {
    page_table_t root = 0;
    cpu = (cpu_local_t){.bsp = true, .online = true};
    online_cpus = 0; /* CPU topology has not yet been published. */
    interrupts_enabled = false;
    assert(vmm_map_kernel_bootstrap(&root, (void *)1, 4096, 3));
    assert(map_calls == 1U);
    interrupts_enabled = true;
    assert(!vmm_map_kernel_bootstrap(&root, (void *)1, 4096, 3));
    online_cpus = 2;
    interrupts_enabled = false;
    assert(!vmm_map_kernel_bootstrap(&root, (void *)1, 4096, 3));
    online_cpus = 1;
    discovered_cpus = 2;
    extra_cpu = (cpu_local_t){.bsp = false, .online = true};
    assert(!vmm_map_kernel_bootstrap(&root, (void *)1, 4096, 3));
    extra_cpu.online = false;
    discovered_cpus = 0;
    cpu.bsp = false;
    assert(!vmm_map_kernel_bootstrap(&root, (void *)1, 4096, 3));
    cpu.bsp = true; cpu.online = false;
    assert(!vmm_map_kernel_bootstrap(&root, (void *)1, 4096, 3));
    assert(map_calls == 1U);
    puts("kernel mapping bootstrap: IF, BSP identity, and online-CPU guards passed");
}
'''
run('vmm-bootstrap-guard', bootstrap_guard)
