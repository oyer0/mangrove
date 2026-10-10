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
for name in ['vmm_user_child_reserved','vmm_user_path_available','vmm_map_user_page']:
    code = function('kernel/src/vmm.c', name)
    code = re.sub(r'__asm__ volatile\("invlpg.*?;', 'invalidate_page(virtual_addr);', code, flags=re.S)
    vmm += code
vmm += r'''
static u64 map(u64 address){u64 frame=(u64)(uintptr_t)aligned_alloc(4096,4096);assert(frame);assert(vmm_map_user_page(meta.root,(void*)(uintptr_t)address,frame,PTE_USER|PTE_READWRITE));assert(!meta.operation_lock.held);return frame;}
int main(void){
 kernel_pml4=aligned_alloc(4096,4096);memset(kernel_pml4,0,4096);meta.root=aligned_alloc(4096,4096);memset(meta.root,0,4096);
 u64 first=map(0x400000);assert(table_allocations==3&&table_frees==0);map(0x401000);assert(table_allocations==3);map(0x600000);assert(table_allocations==4);map(0x40000000);assert(table_allocations==6);map(1ULL<<39);assert(table_allocations==9);
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
