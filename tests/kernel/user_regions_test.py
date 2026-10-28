#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production user-stack/TLS reservation layout and rollback fixture."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "gcc"


def function(name):
    source = (ROOT / "kernel/src/process.c").read_text()
    pattern = re.compile(r"^(?:static )?[A-Za-z_][^\n;{}]*\b" +
                         re.escape(name) + r"\s*\(", re.M)
    for match in pattern.finditer(source):
        opening = source.index("{", match.end())
        if ";" in source[match.end():opening]:
            continue
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise AssertionError(name)


names = (
    "process_size_align_up", "process_address_align_up",
    "process_memory_size_to_pages", "process_memory_find_space",
    "process_user_region_insert_locked", "process_user_register_initial_stack",
    "process_user_region_copy_to_frames", "process_user_region_create",
    "process_user_region_release_unpublished", "process_user_tls_create",
    "process_memory_unmap",
)
production = "\n".join(function(name) for name in names)

fixture = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef uint8_t u8; typedef uint64_t u64; typedef uint32_t u32;
typedef uintptr_t usize; typedef uintptr_t phys_addr_t; typedef int64_t i64;
typedef pthread_mutex_t mutex_t;
typedef enum { PROCESS_STATE_ACTIVE=0, PROCESS_STATE_EXITING=1 } process_state_t;
typedef enum { PROCESS_USER_REGION_INITIAL_STACK=1,
    PROCESS_USER_REGION_WORKER_STACK, PROCESS_USER_REGION_THREAD_TLS
} process_user_region_kind_t;
#define VMM_PAGE_SIZE 4096U
#define VMM_USER_ANON_BASE 0x0000400000000000ULL
#define VMM_USER_ANON_LIMIT 0x00007fff00000000ULL
#define VMM_USER_ADDRESS_LIMIT 0x0000800000000000ULL
#define PROCESS_USER_STACK_TOP 0x00007ffffff00000ULL
#define PROCESS_USER_STACK_PAGES 8U
#define PROCESS_TLS_TCB_BYTES (2U * sizeof(uintptr_t))
#define PTE_USER 4ULL
#define PTE_READWRITE 2ULL
#define PTE_NX (1ULL<<63)
#define MG_OK 0
#define MG_ERR_BAD_ARGUMENT -1
#define MG_ERR_WOULD_BLOCK -2
#define MG_ERR_NOT_FOUND -3
#define MG_ERR_BUSY -4
typedef struct process_memory_mapping {
    uintptr_t address; usize page_count; struct process_memory_mapping *next;
} process_memory_mapping_t;
typedef struct process_user_region {
    uintptr_t reservation_base; usize reservation_pages;
    uintptr_t mapping_base; usize mapping_pages;
    process_user_region_kind_t kind; struct process_user_region *next;
} process_user_region_t;
typedef struct process {
    process_state_t state; bool exit_requested; void *address_space;
    mutex_t memory_lock; process_memory_mapping_t *memory_mappings;
    process_user_region_t *user_regions;
    u8 *tls_image; usize tls_file_size, tls_memory_size, tls_alignment;
    bool tls_present;
} process_t;
typedef struct {uintptr_t address;phys_addr_t frame;} mapping_t;
static mapping_t mappings[512]; static unsigned mapping_count;
static unsigned live_frames, map_calls, fail_map_after;
static int fail_frame_after=-1, fail_malloc_after=-1;
static bool fail_map;
static void *kmalloc(usize n){if(fail_malloc_after==0)return NULL;if(fail_malloc_after>0)fail_malloc_after--;return malloc(n);}
static void kfree(void*p){free(p);}
static bool mutex_lock(mutex_t*m){return pthread_mutex_lock(m)==0;}
static void mutex_unlock(mutex_t*m){assert(pthread_mutex_unlock(m)==0);}
static phys_addr_t pmm_alloc_frame(void){void*p=NULL;if(fail_frame_after==0)return 0;if(fail_frame_after>0)fail_frame_after--;assert(posix_memalign(&p,VMM_PAGE_SIZE,VMM_PAGE_SIZE)==0);memset(p,0xa5,VMM_PAGE_SIZE);live_frames++;return (uintptr_t)p;}
static void pmm_free_frame(phys_addr_t f){assert(f&&live_frames);live_frames--;free((void*)f);}
static bool vmm_direct_map_valid(phys_addr_t f){return f!=0;}
static void *phys_to_virt(phys_addr_t f){return (void*)f;}
static bool vmm_user_range_unmapped(void*as,uintptr_t start,usize length){assert(as&&start%VMM_PAGE_SIZE==0&&length%VMM_PAGE_SIZE==0);for(uintptr_t a=start;a<start+length;a+=VMM_PAGE_SIZE)for(unsigned i=0;i<mapping_count;i++)if(mappings[i].address==a)return false;return true;}
static bool vmm_map_user_page(void*as,void*va,phys_addr_t frame,u64 flags){assert(as&&frame&&(flags&PTE_USER));map_calls++;if(fail_map&&map_calls==fail_map_after)return false;assert(mapping_count<512);uintptr_t a=(uintptr_t)va;for(unsigned i=0;i<mapping_count;i++)assert(mappings[i].address!=a);mappings[mapping_count++]=(mapping_t){a,frame};return true;}
static bool vmm_unmap_user_range(void*as,void*va,usize pages){assert(as);uintptr_t start=(uintptr_t)va;for(usize p=0;p<pages;p++){uintptr_t a=start+p*VMM_PAGE_SIZE;unsigned i;for(i=0;i<mapping_count;i++)if(mappings[i].address==a)break;if(i==mapping_count)return false;phys_addr_t f=mappings[i].frame;mappings[i]=mappings[--mapping_count];pmm_free_frame(f);}return true;}
static uint8_t *mapped_bytes(uintptr_t address){uintptr_t page=address&~(uintptr_t)(VMM_PAGE_SIZE-1U);for(unsigned i=0;i<mapping_count;i++)if(mappings[i].address==page)return (uint8_t*)mappings[i].frame+(address-page);return NULL;}
static void panic(const char*m){fprintf(stderr,"%s\n",m);abort();}
'''+production+r'''
static void reset_failures(void){fail_frame_after=-1;fail_malloc_after=-1;fail_map=false;}
static void test_stack_guard_reservation_and_unmap(void)
{
    process_t p={.state=PROCESS_STATE_ACTIVE,.address_space=(void*)1};
    assert(pthread_mutex_init(&p.memory_lock,NULL)==0);
    /* Simulate the ELF loader's eight mapped initial-stack pages. */
    uintptr_t stack_base=PROCESS_USER_STACK_TOP-8U*VMM_PAGE_SIZE;
    for(unsigned i=0;i<8;i++){phys_addr_t f=pmm_alloc_frame();assert(f);assert(vmm_map_user_page(p.address_space,(void*)(stack_base+i*VMM_PAGE_SIZE),f,PTE_USER|PTE_READWRITE|PTE_NX));}
    assert(process_user_register_initial_stack(&p,PROCESS_USER_STACK_TOP,8U));
    assert(p.user_regions&&p.user_regions->kind==PROCESS_USER_REGION_INITIAL_STACK);
    assert(vmm_user_range_unmapped(p.address_space,stack_base-VMM_PAGE_SIZE,VMM_PAGE_SIZE));
    assert(process_memory_unmap(&p,stack_base)==MG_ERR_NOT_FOUND);
    phys_addr_t elf_frame=pmm_alloc_frame();assert(elf_frame);
    assert(vmm_map_user_page(p.address_space,(void*)VMM_USER_ANON_BASE,
                             elf_frame,PTE_USER|PTE_NX));
    uintptr_t first=process_memory_find_space(&p,2U);
    assert(first==VMM_USER_ANON_BASE+VMM_PAGE_SIZE);
    uintptr_t stack_region_pointer=0;
    process_user_region_t *a=process_user_region_create(&p,
        PROCESS_USER_REGION_WORKER_STACK,1U,8U,VMM_PAGE_SIZE,NULL,0,0,0,
        &stack_region_pointer);
    assert(a&&a->reservation_base==first);
    assert(vmm_user_range_unmapped(p.address_space,a->reservation_base,VMM_PAGE_SIZE));
    assert(process_memory_unmap(&p,a->mapping_base)==MG_ERR_NOT_FOUND);
    process_user_region_t *b=process_user_region_create(&p,
        PROCESS_USER_REGION_WORKER_STACK,1U,8U,VMM_PAGE_SIZE,NULL,0,0,0,NULL);
    assert(b&&b->reservation_base==a->reservation_base+9U*VMM_PAGE_SIZE);
    assert(a->reservation_base+a->reservation_pages*VMM_PAGE_SIZE<=b->reservation_base);
    assert(process_user_region_release_unpublished(&p,b));
    assert(process_user_region_release_unpublished(&p,a));
    assert(p.user_regions&&p.user_regions->kind==PROCESS_USER_REGION_INITIAL_STACK&&
           !p.user_regions->next);
    assert(vmm_unmap_user_range(p.address_space,(void*)stack_base,8U));
    assert(vmm_unmap_user_range(p.address_space,(void*)VMM_USER_ANON_BASE,1U));
    kfree(p.user_regions);p.user_regions=NULL;
    assert(live_frames==0);
    assert(pthread_mutex_destroy(&p.memory_lock)==0);
}
static void test_tls_image_zero_fill_and_instances(void)
{
    process_t p={.state=PROCESS_STATE_ACTIVE,.address_space=(void*)1,
        .tls_present=true,.tls_file_size=5,.tls_memory_size=13,
        .tls_alignment=64};
    u8 image[5]={0x31,0x42,0x53,0x64,0x75};p.tls_image=image;
    assert(pthread_mutex_init(&p.memory_lock,NULL)==0);
    uintptr_t start_a=0,start_b=0;
    process_user_region_t *a=process_user_tls_create(&p,&start_a);
    process_user_region_t *b=process_user_tls_create(&p,&start_b);
    assert(a&&b&&start_a!=start_b&&a!=b);
    assert(start_a==a->mapping_base+64U&&start_b==b->mapping_base+64U);
    assert(start_a%64U==0&&start_b%64U==0);
    for(unsigned i=0;i<5;i++)assert(*mapped_bytes(start_a-64U+i)==image[i]);
    for(unsigned i=5;i<64;i++)assert(*mapped_bytes(start_a-64U+i)==0);
    uintptr_t self_a=0,self_b=1;
    memcpy(&self_a,mapped_bytes(start_a),sizeof(self_a));
    memcpy(&self_b,mapped_bytes(start_a+sizeof(uintptr_t)),sizeof(self_b));
    assert(self_a==start_a&&self_b==0);
    assert(vmm_user_range_unmapped(p.address_space,a->reservation_base,VMM_PAGE_SIZE));
    assert(process_user_region_release_unpublished(&p,b));
    assert(process_user_region_release_unpublished(&p,a));
    assert(live_frames==0&&mapping_count==0);
    assert(pthread_mutex_destroy(&p.memory_lock)==0);
}
static void test_tls_allocation_failures_roll_back(void)
{
    u8 image[4]={1,2,3,4};
    process_t p={.state=PROCESS_STATE_ACTIVE,.address_space=(void*)1,
        .tls_present=true,.tls_image=image,.tls_file_size=sizeof(image),
        .tls_memory_size=8,.tls_alignment=64};
    uintptr_t fs_base=0;
    assert(pthread_mutex_init(&p.memory_lock,NULL)==0);
    fail_malloc_after=0;
    assert(!process_user_tls_create(&p,&fs_base));
    reset_failures();
    assert(!p.user_regions&&live_frames==0&&mapping_count==0);
    fail_malloc_after=1;
    assert(!process_user_tls_create(&p,&fs_base));
    reset_failures();
    assert(!p.user_regions&&live_frames==0&&mapping_count==0);
    fail_frame_after=0;
    assert(!process_user_tls_create(&p,&fs_base));
    reset_failures();
    assert(!p.user_regions&&live_frames==0&&mapping_count==0);
    fail_map=true;map_calls=0;fail_map_after=1;
    assert(!process_user_tls_create(&p,&fs_base));
    reset_failures();
    assert(!p.user_regions&&live_frames==0&&mapping_count==0);
    assert(pthread_mutex_destroy(&p.memory_lock)==0);
}
static void test_allocation_failures_roll_back(void)
{
    process_t p={.state=PROCESS_STATE_ACTIVE,.address_space=(void*)1};
    assert(pthread_mutex_init(&p.memory_lock,NULL)==0);
    fail_malloc_after=1;
    assert(!process_user_region_create(&p,PROCESS_USER_REGION_WORKER_STACK,
        1U,8U,VMM_PAGE_SIZE,NULL,0,0,0,NULL));
    reset_failures();assert(!p.user_regions&&live_frames==0&&mapping_count==0);
    fail_frame_after=3;
    assert(!process_user_region_create(&p,PROCESS_USER_REGION_WORKER_STACK,
        1U,8U,VMM_PAGE_SIZE,NULL,0,0,0,NULL));
    reset_failures();assert(!p.user_regions&&live_frames==0&&mapping_count==0);
    fail_map=true;map_calls=0;fail_map_after=4;
    assert(!process_user_region_create(&p,PROCESS_USER_REGION_WORKER_STACK,
        1U,8U,VMM_PAGE_SIZE,NULL,0,0,0,NULL));
    reset_failures();assert(!p.user_regions&&live_frames==0&&mapping_count==0);
    assert(pthread_mutex_destroy(&p.memory_lock)==0);
}
int main(void)
{
    test_stack_guard_reservation_and_unmap();
    test_tls_image_zero_fill_and_instances();
    test_tls_allocation_failures_roll_back();
    test_allocation_failures_roll_back();
    puts("guarded user-stack reservations, TLS instances and rollback PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-user-regions-") as folder:
    folder = Path(folder)
    source = folder / "user_regions_test.c"
    binary = folder / "user_regions_test"
    source.write_text(fixture)
    subprocess.run([CC, "-std=gnu17", "-O1", "-g", "-Wall", "-Wextra",
        "-Werror", "-fsanitize=address,undefined", "-pthread", str(source),
        "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True,
        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
