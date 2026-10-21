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


def function_definition(path, name):
    source = (ROOT / path).read_text()
    pattern = re.compile(r'^(?:static )?[A-Za-z_][^\n;{}]*\b' +
                         re.escape(name) + r'\s*\(', re.M)
    for match in pattern.finditer(source):
        open_paren = source.index('(', match.start())
        depth = 1
        end_args = open_paren + 1
        while depth:
            depth += (source[end_args] == '(') - (source[end_args] == ')')
            end_args += 1
        brace = end_args
        while source[brace].isspace():
            brace += 1
        if source[brace] != '{':
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end] + '\n'
    raise AssertionError(name)


COMMON = r'''
#define _GNU_SOURCE
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
#define KERNEL_VMM_RANGE_BASE 0xffffc10000000000ULL
#define KERNEL_VMM_RANGE_LIMIT 0xffffc18000000000ULL
#define VMM_PAGE_SIZE 4096ULL
#define VMM_KERNEL_RANGE_PML4_INDEX ((KERNEL_VMM_RANGE_BASE >> 39) & 0x1ffU)
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
static bool range_map_allowed;
static unsigned range_mapping_commits;
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
           index == VMM_KERNEL_RANGE_PML4_INDEX ||
           index == VMM_IMAGE_PML4_INDEX;
}
static bool vmm_kernel_range_authorizes_mapping_locked(u64 address,
                                                        u32 *record_index) {
    bool allowed = range_map_allowed &&
                   address >= KERNEL_VMM_RANGE_BASE &&
                   address < KERNEL_VMM_RANGE_LIMIT &&
                   !(address & (VMM_PAGE_SIZE - 1U));
    if (allowed) *record_index = 0U;
    return allowed;
}
static void vmm_kernel_range_mapping_committed_locked(u32 record_index) {
    assert(record_index == 0U);
    range_mapping_commits++;
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
    assert(!vmm_map_kernel_one_internal(kernel_pml4,
                (void *)(uintptr_t)KERNEL_VMM_RANGE_BASE, 0x12345000U,
                PTE_PRESENT | PTE_READWRITE, false));
    assert(table_attempts == 0U &&
           kernel_pml4->entries[VMM_KERNEL_RANGE_PML4_INDEX] == 0U);
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
    assert(!vmm_map_kernel_one_internal(kernel_pml4,
                (void *)(uintptr_t)KERNEL_VMM_RANGE_BASE, 0x12345000U,
                PTE_PRESENT | PTE_READWRITE, false));
    assert(table_allocations == 0U);
    range_map_allowed = true;
    assert(!vmm_map_kernel_one_internal(kernel_pml4,
                (void *)(uintptr_t)KERNEL_VMM_RANGE_BASE, 0x12345001U,
                PTE_PRESENT | PTE_READWRITE, false));
    assert(vmm_map_kernel_one_internal(kernel_pml4,
                (void *)(uintptr_t)KERNEL_VMM_RANGE_BASE, 0x12345000U,
                PTE_PRESENT | PTE_READWRITE, false));
    assert(table_allocations == 3U &&
           kernel_pml4->entries[VMM_KERNEL_RANGE_PML4_INDEX] != 0U);
    assert(range_mapping_commits == 1U);
    assert(!vmm_map_kernel_one_internal(kernel_pml4,
                (void *)(uintptr_t)KERNEL_VMM_RANGE_BASE, 0x22345000U,
                PTE_PRESENT | PTE_READWRITE, false));
    assert(range_mapping_commits == 1U);
    release_tables_and_root();
    range_map_allowed = false;

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


kernel_range = r'''
#include <pthread.h>
#include <stdatomic.h>
#define VMM_PAGE_SIZE 4096ULL
#define PTE_PRESENT 1ULL
#define PTE_READWRITE 2ULL
#define PTE_USER 4ULL
#define PTE_HUGE 128ULL
#define PTE_NX (1ULL << 63)
#define PTE_FRAME_MASK 0x000ffffffffff000ULL
#define KERNEL_VMM_RANGE_BASE 0xffffc10000000000ULL
#define KERNEL_VMM_RANGE_LIMIT 0xffffc18000000000ULL
#define PHYS_MAP_BASE 0xffff800000000000ULL
#define PHYS_MAP_LIMIT 0x0000400000000000ULL
#define KERNEL_HEAP_BASE 0xffffc00000000000ULL
#define IOREMAP_BASE 0xffffc08000000000ULL
#define KERNEL_VIRT_BASE 0xffffffff80000000ULL
#define VMM_DIRECT_MAP_PML4_FIRST ((PHYS_MAP_BASE >> 39) & 0x1ffU)
#define VMM_DIRECT_MAP_PML4_LAST \
    (((PHYS_MAP_BASE + PHYS_MAP_LIMIT - 1U) >> 39) & 0x1ffU)
#define VMM_HEAP_PML4_INDEX ((KERNEL_HEAP_BASE >> 39) & 0x1ffU)
#define VMM_IOREMAP_PML4_INDEX ((IOREMAP_BASE >> 39) & 0x1ffU)
#define VMM_IMAGE_PML4_INDEX ((KERNEL_VIRT_BASE >> 39) & 0x1ffU)
typedef uintptr_t virt_addr_t;
typedef u64 phys_addr_t;
typedef struct { u64 entries[512]; } __attribute__((aligned(4096))) page_table_t;
typedef struct {
    virt_addr_t base;
    u64 page_count;
    u64 generation;
} vmm_kernel_range_t;
#define VMM_KERNEL_RANGE_RESERVATION_CAPACITY 128U
#define VMM_KERNEL_RANGE_PML4_INDEX \
    ((KERNEL_VMM_RANGE_BASE >> 39) & 0x1ffU)
typedef pthread_mutex_t spinlock_t;
typedef struct {
    virt_addr_t base;
    u64 page_count;
    u64 mapped_pages;
    u64 generation;
} vmm_kernel_range_record_t;
static page_table_t *kernel_pml4;
static spinlock_t vmm_kernel_lock = PTHREAD_MUTEX_INITIALIZER;
static spinlock_t vmm_kernel_range_lock = PTHREAD_MUTEX_INITIALIZER;
static vmm_kernel_range_record_t vmm_kernel_ranges[
    VMM_KERNEL_RANGE_RESERVATION_CAPACITY];
static u64 vmm_kernel_range_next_generation = 1U;
static bool vmm_kernel_range_initialized = true;
static u64 spin_lock_irqsave(spinlock_t *lock) {
    assert(pthread_mutex_lock(lock) == 0); return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    (void)flags; assert(pthread_mutex_unlock(lock) == 0);
}
static void panic(const char *message) {
    fprintf(stderr, "%s\n", message); abort();
}
static page_table_t *vmm_table_from_phys(phys_addr_t physical) {
    return (page_table_t *)(uintptr_t)(physical & PTE_FRAME_MASK);
}
'''
for name in ['vmm_kernel_shared_pml4_index',
             'vmm_kernel_range_record_empty',
             'vmm_kernel_range_record_valid',
             'vmm_kernel_range_mapping_end_locked',
             'vmm_kernel_range_authorizes_mapping_locked',
             'vmm_kernel_range_mapping_committed_locked',
             'vmm_kernel_range_reserve', 'vmm_kernel_range_release']:
    kernel_range += function_definition('kernel/src/vmm.c', name)
kernel_range += r'''
static u64 table_entry(page_table_t *table) {
    return ((u64)(uintptr_t)table & PTE_FRAME_MASK) |
           PTE_PRESENT | PTE_READWRITE;
}
static void test_basic_and_stale_handles(void) {
    vmm_kernel_range_t first = {0}, second = {0}, replacement = {0};
    vmm_kernel_range_t stale;
    assert(vmm_kernel_range_reserve(1, &first));
    assert(first.base == KERNEL_VMM_RANGE_BASE && first.page_count == 1 &&
           first.generation != 0 && !(first.base & (VMM_PAGE_SIZE - 1)));
    assert(kernel_pml4->entries[VMM_KERNEL_RANGE_PML4_INDEX] == 0);
    u32 owner = ~(u32)0;
    assert(vmm_kernel_range_authorizes_mapping_locked(first.base, &owner));
    assert(owner < VMM_KERNEL_RANGE_RESERVATION_CAPACITY);
    assert(!vmm_kernel_range_authorizes_mapping_locked(first.base + 1U,
                                                       &owner));
    assert(!vmm_kernel_range_authorizes_mapping_locked(
        first.base + VMM_PAGE_SIZE, &owner));
    stale = first;
    assert(vmm_kernel_range_reserve(2, &second));
    assert(second.base == first.base + VMM_PAGE_SIZE);
    assert(vmm_kernel_range_release(&first));
    assert(first.base == 0 && first.page_count == 0 && first.generation == 0);
    assert(!vmm_kernel_range_release(&first));
    assert(vmm_kernel_range_reserve(1, &replacement));
    assert(replacement.base == stale.base &&
           replacement.generation != stale.generation);
    assert(!vmm_kernel_range_release(&stale));
    assert(vmm_kernel_range_release(&replacement));
    assert(vmm_kernel_range_release(&second));
}
static void test_rejection_and_capacity(void) {
    vmm_kernel_range_t invalid = {0}, held[128] = {{0}};
    vmm_kernel_range_t occupied = {0};
    bool was_initialized = vmm_kernel_range_initialized;
    page_table_t *saved_root = kernel_pml4;
    vmm_kernel_range_initialized = false;
    assert(!vmm_kernel_range_reserve(1, &invalid));
    vmm_kernel_range_initialized = was_initialized;
    kernel_pml4 = NULL;
    assert(!vmm_kernel_range_reserve(1, &invalid));
    kernel_pml4 = saved_root;
    assert(!vmm_kernel_range_reserve(0, &invalid));
    assert(!vmm_kernel_range_reserve(~(u64)0, &invalid));
    invalid.base = 1;
    assert(!vmm_kernel_range_reserve(1, &invalid));
    invalid = (vmm_kernel_range_t){0};
    assert(!vmm_kernel_range_release(&invalid));
    invalid = (vmm_kernel_range_t){
        .base = KERNEL_VMM_RANGE_BASE + 1U,
        .page_count = 1,
        .generation = 1
    };
    assert(!vmm_kernel_range_release(&invalid));
    invalid = (vmm_kernel_range_t){
        .base = KERNEL_VMM_RANGE_LIMIT,
        .page_count = 1,
        .generation = 1
    };
    assert(!vmm_kernel_range_release(&invalid));
    invalid = (vmm_kernel_range_t){
        .base = KERNEL_VMM_RANGE_BASE,
        .page_count = ~(u64)0,
        .generation = 1
    };
    assert(!vmm_kernel_range_release(&invalid));
    invalid = (vmm_kernel_range_t){
        .base = KERNEL_VMM_RANGE_BASE,
        .page_count = (KERNEL_VMM_RANGE_LIMIT - KERNEL_VMM_RANGE_BASE) /
                      VMM_PAGE_SIZE + 1U,
        .generation = 1
    };
    assert(!vmm_kernel_range_release(&invalid));
    invalid = (vmm_kernel_range_t){0};
    assert(!vmm_kernel_range_reserve(1, NULL));
    for (u32 i = 0; i < 128; i++)
        assert(vmm_kernel_range_reserve(1, &held[i]));
    assert(!vmm_kernel_range_reserve(1, &occupied));
    for (u32 i = 0; i < 128; i++)
        assert(vmm_kernel_range_release(&held[i]));
    assert(vmm_kernel_range_reserve(
        (KERNEL_VMM_RANGE_LIMIT - KERNEL_VMM_RANGE_BASE) / VMM_PAGE_SIZE,
        &occupied));
    assert(occupied.base == KERNEL_VMM_RANGE_BASE);
    assert(!vmm_kernel_range_reserve(1, &invalid));
    assert(vmm_kernel_range_release(&occupied));
}
static void test_live_mapping_blocks_release(void) {
    vmm_kernel_range_t range = {0};
    page_table_t *pdpt, *pd, *pt;
    virt_addr_t address;
    u32 owner = ~(u32)0;
    assert(vmm_kernel_range_reserve(2, &range));
    address = range.base + VMM_PAGE_SIZE;
    pdpt = aligned_alloc(VMM_PAGE_SIZE, VMM_PAGE_SIZE);
    pd = aligned_alloc(VMM_PAGE_SIZE, VMM_PAGE_SIZE);
    pt = aligned_alloc(VMM_PAGE_SIZE, VMM_PAGE_SIZE);
    assert(pdpt && pd && pt);
    memset(pdpt, 0, VMM_PAGE_SIZE);
    memset(pd, 0, VMM_PAGE_SIZE);
    memset(pt, 0, VMM_PAGE_SIZE);
    kernel_pml4->entries[VMM_KERNEL_RANGE_PML4_INDEX] = table_entry(pdpt);
    pdpt->entries[(address >> 30) & 0x1ffU] = table_entry(pd);
    pd->entries[(address >> 21) & 0x1ffU] = table_entry(pt);
    pt->entries[(address >> 12) & 0x1ffU] = 0x5000U | PTE_PRESENT | PTE_NX;
    assert(vmm_kernel_range_authorizes_mapping_locked(address, &owner));
    vmm_kernel_range_mapping_committed_locked(owner);
    assert(!vmm_kernel_range_release(&range));
    assert(range.generation != 0);
    pt->entries[(address >> 12) & 0x1ffU] = 0;
    pt->entries[(range.base >> 12) & 0x1ffU] =
        0x6000U | PTE_PRESENT | PTE_NX;
    assert(vmm_kernel_range_authorizes_mapping_locked(range.base, &owner));
    vmm_kernel_range_mapping_committed_locked(owner);
    assert(!vmm_kernel_range_authorizes_mapping_locked(range.base, &owner));
    pt->entries[(range.base >> 12) & 0x1ffU] = 0;
    /* Without a VMM unmap/TLB-quiescence operation, a mapped lease remains
     * owned even after this fixture removes its synthetic leaf by hand. */
    assert(!vmm_kernel_range_release(&range));
    kernel_pml4->entries[VMM_KERNEL_RANGE_PML4_INDEX] = 0;
    free(pdpt); free(pd); free(pt);
}
static void test_corrupt_lease_records_fail_closed(void) {
    vmm_kernel_range_t range = {0}, other = {0};
    vmm_kernel_range_record_t saved;
    u32 owner = ~(u32)0;
    assert(vmm_kernel_range_reserve(1, &range));
    assert(vmm_kernel_range_authorizes_mapping_locked(range.base, &owner));
    saved = vmm_kernel_ranges[owner];
    vmm_kernel_ranges[owner].base = KERNEL_VMM_RANGE_LIMIT;
    assert(!vmm_kernel_range_authorizes_mapping_locked(range.base, &owner));
    assert(!vmm_kernel_range_reserve(1, &other));
    assert(!vmm_kernel_range_release(&range));
    vmm_kernel_ranges[owner] = saved;
    vmm_kernel_ranges[owner].generation = 0;
    assert(!vmm_kernel_range_authorizes_mapping_locked(range.base, &owner));
    assert(!vmm_kernel_range_reserve(1, &other));
    vmm_kernel_ranges[owner] = saved;
    assert(vmm_kernel_range_release(&range));
}
typedef struct {
    pthread_barrier_t *barrier;
    vmm_kernel_range_t *ranges;
    u32 index;
} worker_arg_t;
static void *reservation_worker(void *opaque) {
    worker_arg_t *arg = opaque;
    for (u32 round = 0; round < 40; round++) {
        vmm_kernel_range_t *range = &arg->ranges[arg->index];
        assert(vmm_kernel_range_reserve(3, range));
        int barrier_result = pthread_barrier_wait(arg->barrier);
        assert(barrier_result == 0 ||
               barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);
        for (u32 other = 0; other < 4; other++) {
            if (other == arg->index) continue;
            vmm_kernel_range_t *peer = &arg->ranges[other];
            assert(range->base + range->page_count * VMM_PAGE_SIZE <= peer->base ||
                   peer->base + peer->page_count * VMM_PAGE_SIZE <= range->base);
        }
        (void)pthread_barrier_wait(arg->barrier);
        assert(vmm_kernel_range_release(range));
        (void)pthread_barrier_wait(arg->barrier);
    }
    return NULL;
}
static void test_concurrent_reservations(void) {
    pthread_t threads[4];
    pthread_barrier_t barrier;
    vmm_kernel_range_t ranges[4] = {{0}};
    worker_arg_t args[4];
    assert(pthread_barrier_init(&barrier, NULL, 4) == 0);
    for (u32 i = 0; i < 4; i++) {
        args[i] = (worker_arg_t){&barrier, ranges, i};
        assert(pthread_create(&threads[i], NULL, reservation_worker,
                              &args[i]) == 0);
    }
    for (u32 i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(pthread_barrier_destroy(&barrier) == 0);
}
static void test_generation_exhaustion(void) {
    vmm_kernel_range_t last = {0}, rejected = {0};
    vmm_kernel_range_next_generation = ~(u64)0;
    assert(vmm_kernel_range_reserve(1, &last));
    assert(last.generation == ~(u64)0);
    assert(vmm_kernel_range_release(&last));
    assert(!vmm_kernel_range_reserve(1, &rejected));
}
int main(void) {
    kernel_pml4 = aligned_alloc(VMM_PAGE_SIZE, VMM_PAGE_SIZE);
    assert(kernel_pml4);
    memset(kernel_pml4, 0, VMM_PAGE_SIZE);
    assert(vmm_kernel_shared_pml4_index(VMM_KERNEL_RANGE_PML4_INDEX));
    test_basic_and_stale_handles();
    test_rejection_and_capacity();
    test_live_mapping_blocks_release();
    test_corrupt_lease_records_fail_closed();
    test_concurrent_reservations();
    test_generation_exhaustion();
    free(kernel_pml4);
    puts("kernel virtual ranges: leases, generations, bounds, mapped-release rejection, capacity and concurrency passed");
}
'''
run('vmm-kernel-range', kernel_range)
