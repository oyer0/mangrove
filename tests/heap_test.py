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


heap = r'''
typedef u64 phys_addr_t;typedef int spinlock_t;typedef int mutex_t;typedef struct {int unused;} page_table_t;
typedef struct heap_block {usize size;bool free;struct heap_block*next,*prev;} heap_block_t;
typedef struct {void *start,*end;heap_block_t*first;} heap_t;
static heap_t kernel_heap;static spinlock_t heap_lock;static mutex_t heap_growth_lock;
#define HEAP_INITIAL_PAGES 4
#define heap_verify(where) true
static u64 spin_lock_irqsave(spinlock_t*l){assert(!*l);*l=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t*l,u64 flags){(void)flags;assert(*l);*l=0;}
static bool mutex_lock(mutex_t*m){(void)m;return true;}
static void mutex_unlock(mutex_t*m){(void)m;}
static u64 pmm_alloc_frame(void){return 0;}
static void pmm_free_frame(u64 p){(void)p;}
static bool heap_grow_reserved(const u64*f,u32*c){(void)f;*c=0;return false;}
static void kprint(const char*f,...){(void)f;}
static void panic(const char*s){fprintf(stderr,"%s\n",s);abort();}
'''
for name in ['heap_header_valid','heap_block_valid','kmalloc','kfree']:
    heap += function('kernel/src/heap.c', name)
heap += r'''
int main(void){
 void *memory=aligned_alloc(16,65536);kernel_heap=(heap_t){memory,(u8*)memory+65536,memory};*kernel_heap.first=(heap_block_t){65536-sizeof(heap_block_t),true,NULL,NULL};
 void *a=kmalloc(17),*b=kmalloc(100),*c=kmalloc(4096);assert(a&&b&&c&&((uintptr_t)a%16)==0);memset(a,0x5a,17);memset(b,0x6b,100);assert(heap_block_valid(kernel_heap.first));
 kfree(b);kfree(a);kfree(c);assert(kernel_heap.first->free&&!kernel_heap.first->next&&kernel_heap.first->size==65536-sizeof(heap_block_t));
 assert(!kmalloc(0)&&!kmalloc((usize)-1));assert(!heap_header_valid((heap_block_t*)((u8*)memory+65520)));assert(!heap_header_valid((heap_block_t*)((u8*)memory+1)));
 a=kmalloc(16);heap_block_t *header=(heap_block_t*)((u8*)a-sizeof(heap_block_t));heap_block_t *next=header->next;header->next=(heap_block_t*)((u8*)kernel_heap.end+16);assert(!heap_block_valid(header));header->next=next;next->prev=NULL;assert(!heap_block_valid(header));next->prev=header;kfree(a);free(memory);
 puts("heap: aligned allocation, splitting/coalescing, overflow, bounds and neighbour corruption checks passed");
}
'''


run('heap', heap)
