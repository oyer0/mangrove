#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare historical first-fit and current indexed Pith heap code on the host.

This is a development measurement, not a host performance claim about Pith.
It extracts allocation/free functions from the two repository revisions and
links them to the same small pthread/PMM/VMM adapter and deterministic workloads.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
BASELINE = "3288152ffffe130ece618942a0c63612f98989b0"
CURRENT = "working-tree"
COMMON_C = r'''
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
typedef size_t usize; typedef uint64_t phys_addr_t;
typedef struct { pthread_mutex_t native; } spinlock_t;
typedef struct { pthread_mutex_t native; } mutex_t;
typedef struct { int unused; } page_table_t;
#if INDEXED
typedef struct heap_block {
    usize size; bool free, in_free_bin;
    struct heap_block *next, *prev, *free_next, *free_prev;
} heap_block_t;
typedef struct { void *start, *end; heap_block_t *first, *last; } heap_t;
#else
typedef struct heap_block {
    usize size; bool free;
    struct heap_block *next, *prev;
} heap_block_t;
typedef struct { void *start, *end; heap_block_t *first; } heap_t;
#endif
static heap_t kernel_heap;
static spinlock_t heap_lock;
static mutex_t heap_growth_lock;
#if INDEXED
#define HEAP_FREE_BIN_COUNT 32U
static heap_block_t *heap_free_bins[HEAP_FREE_BIN_COUNT];
static u32 heap_free_bin_bitmap;
static u64 heap_total_payload, heap_used_payload, heap_free_payload;
static bool heap_bootstrap_growth_open;
#endif
static uintptr_t fake_heap_base;
static usize fake_heap_capacity;
#define HEAP_START fake_heap_base
#define HEAP_LIMIT (fake_heap_base + fake_heap_capacity)
#define HEAP_INITIAL_PAGES 4U
#define PAGE_SIZE 4096U
#define PTE_PRESENT 1U
#define PTE_READWRITE 2U
#define PTE_NX (1ULL << 63)
static page_table_t fake_pml4;
static _Atomic u64 pmm_alloc_calls, pmm_free_calls, vmm_map_calls;
static _Atomic u64 vmm_successful_maps;
#if WORK_COUNTER
static u64 bench_search_visits;
#endif
#ifndef LOCK_METRICS
#define LOCK_METRICS 0
#endif
static _Thread_local bool fixture_interrupts_enabled = true;
static _Thread_local u64 fixture_lock_started;
static _Atomic u64 lock_hold_total, lock_hold_max;
static _Atomic u64 lock_hold_count;
static bool lock_metrics_enabled;
static u64 tsc_ns_scale_q32;
static int fixture_cpus[CPU_SETSIZE];
static unsigned fixture_cpu_count;
_Static_assert(sizeof(heap_block_t) == (INDEXED ? 48U : 32U),
               "benchmark block layout must match the selected revision");
static bool heap_header_valid(const heap_block_t *block);
static bool heap_block_valid(const heap_block_t *block);
static void heap_free_insert_locked(heap_block_t *block);
static heap_block_t *heap_free_find_locked(usize size);
static void kprint(const char *format, ...) { (void)format; }
static bool heap_verify(const char *where) { (void)where; return true; }
static u64 clock_ns(void) {
    struct timespec value;
#ifdef CLOCK_MONOTONIC_RAW
    assert(clock_gettime(CLOCK_MONOTONIC_RAW, &value) == 0);
#else
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
#endif
    return (u64)value.tv_sec * 1000000000ULL + (u64)value.tv_nsec;
}
static u64 tsc_ticks(void) {
    u32 low, high, auxiliary;
    __asm__ volatile("lfence\n\trdtscp\n\tlfence"
                     : "=a"(low), "=d"(high), "=c"(auxiliary)
                     : : "memory");
    (void)auxiliary;
    return ((u64)high << 32) | low;
}
static u64 tsc_ticks_to_ns(u64 ticks) {
    return (ticks * tsc_ns_scale_q32) >> 32;
}
static void calibrate_tsc(void) {
    u64 start_ns = clock_ns();
    u64 start_ticks = tsc_ticks();
    while (clock_ns() - start_ns < 100000000ULL) { }
    u64 end_ticks = tsc_ticks();
    u64 elapsed_ns = clock_ns() - start_ns;
    assert(end_ticks > start_ticks && elapsed_ns >= 90000000ULL);
    tsc_ns_scale_q32 = (elapsed_ns << 32) / (end_ticks - start_ticks);
    assert(tsc_ns_scale_q32 != 0);
}
static int compare_u64(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return (x > y) - (x < y);
}
static u64 spin_lock_irqsave(spinlock_t *lock) {
    bool enabled = fixture_interrupts_enabled;
    fixture_interrupts_enabled = false;
    assert(pthread_mutex_lock(&lock->native) == 0);
    if (lock_metrics_enabled) fixture_lock_started = tsc_ticks();
    return enabled ? (1ULL << 9) : 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    if (lock_metrics_enabled && lock == &heap_lock) {
        u64 elapsed = tsc_ticks_to_ns(tsc_ticks() - fixture_lock_started);
        atomic_fetch_add(&lock_hold_total, elapsed);
        atomic_fetch_add(&lock_hold_count, 1U);
        u64 prior;
        prior = atomic_load(&lock_hold_max);
        while (elapsed > prior && !atomic_compare_exchange_weak(
                   &lock_hold_max, &prior, elapsed)) { }
    }
    assert(pthread_mutex_unlock(&lock->native) == 0);
    fixture_interrupts_enabled = (flags & (1ULL << 9)) != 0;
}
static bool mutex_lock(mutex_t *mutex) {
    return pthread_mutex_lock(&mutex->native) == 0;
}
static void mutex_unlock(mutex_t *mutex) {
    assert(pthread_mutex_unlock(&mutex->native) == 0);
}
static u32 spinlock_depth_current(void) { return 0; }
static u64 pmm_alloc_frame(void) {
    return (atomic_fetch_add(&pmm_alloc_calls, 1U) + 1U) * PAGE_SIZE;
}
static void pmm_free_frame(phys_addr_t frame) {
    assert(frame != 0);
    atomic_fetch_add(&pmm_free_calls, 1U);
}
static page_table_t *vmm_get_kernel_pml4(void) { return &fake_pml4; }
static bool vmm_map(page_table_t *pml4, void *address, phys_addr_t frame,
                    u64 flags) {
    uintptr_t at = (uintptr_t)address;
    assert(pml4 == &fake_pml4 && frame != 0);
    assert((flags & (PTE_PRESENT | PTE_READWRITE)) ==
           (PTE_PRESENT | PTE_READWRITE));
    assert(at >= fake_heap_base && at + PAGE_SIZE <= HEAP_LIMIT);
    assert((at & (PAGE_SIZE - 1U)) == 0);
    atomic_fetch_add(&vmm_map_calls, 1U);
    atomic_fetch_add(&vmm_successful_maps, 1U);
    return true;
}
static bool vmm_map_kernel_bootstrap(page_table_t *pml4, void *address,
                                     phys_addr_t frame, u64 flags) {
    assert(!fixture_interrupts_enabled);
    return vmm_map(pml4, address, frame, flags);
}
static void panic(const char *message) {
    fprintf(stderr, "unexpected allocator panic: %s\n", message);
    abort();
}
'''


def extract(source: str, name: str) -> str:
    import re
    pattern = re.compile(
        r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + re.escape(name) + r"\s*\(",
        re.M,
    )
    for match in pattern.finditer(source):
        brace = source.find("{", match.end())
        semicolon = source.find(";", match.end())
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise ValueError(f"function definition not found: {name}")


def production_source(source: str, indexed: bool) -> str:
    names = (
        ("heap_bin_index", "heap_maximum_blocks_locked", "heap_header_valid",
         "heap_block_valid", "heap_free_insert_locked", "heap_free_remove_locked",
         "heap_free_find_locked", "heap_grow_reserved", "kmalloc", "kfree",
         "heap_get_total_size", "heap_get_used_size", "heap_get_free_size")
        if indexed else
        ("heap_header_valid", "heap_block_valid", "heap_grow_reserved", "kmalloc", "kfree",
         "heap_get_total_size", "heap_get_used_size", "heap_get_free_size")
    )
    return "\n".join(extract(source, name) for name in names)


def instrument_search(source: str, indexed: bool) -> str:
    """Count allocator search visits in a separate, non-timed probe build."""
    if indexed:
        old = "for (block = heap_free_bins[index]; block;\n             block = block->free_next) {"
    else:
        old = "for (current = kernel_heap.first; current; current = current->next) {"
    new = old + "\n#if WORK_COUNTER\n            bench_search_visits++;\n#endif"
    if source.count(old) < 1:
        raise ValueError("could not find production search loop")
    return source.replace(old, new, 1)


HARNESS_C = r'''
#ifndef SAMPLES
#define SAMPLES 256U
#endif
static _Atomic u64 live_requested;
static void fixture_init(usize capacity, usize initially_mapped, bool locks) {
    void *memory = NULL;
    assert(posix_memalign(&memory, PAGE_SIZE, capacity) == 0 && memory);
    assert(pthread_mutex_init(&heap_lock.native, NULL) == 0);
    assert(pthread_mutex_init(&heap_growth_lock.native, NULL) == 0);
    fake_heap_base = (uintptr_t)memory;
    fake_heap_capacity = capacity;
    if (!initially_mapped) initially_mapped = HEAP_INITIAL_PAGES * PAGE_SIZE;
    assert(initially_mapped >= HEAP_INITIAL_PAGES * PAGE_SIZE &&
           initially_mapped <= capacity && !(initially_mapped & (PAGE_SIZE-1U)));
    kernel_heap.start = memory;
    kernel_heap.end = (u8 *)memory + initially_mapped;
    kernel_heap.first = (heap_block_t *)memory;
    *kernel_heap.first = (heap_block_t){
        .size = initially_mapped - sizeof(heap_block_t), .free = true,
#if INDEXED
        .in_free_bin = false,
#endif
    };
#if INDEXED
    kernel_heap.last = kernel_heap.first;
    memset(heap_free_bins, 0, sizeof(heap_free_bins));
    heap_free_bin_bitmap = 0;
    heap_total_payload = kernel_heap.first->size;
    heap_used_payload = 0;
    heap_free_payload = kernel_heap.first->size;
    heap_bootstrap_growth_open = true;
    heap_free_insert_locked(kernel_heap.first);
#endif
    live_requested = 0;
    atomic_store(&pmm_alloc_calls, 0); atomic_store(&pmm_free_calls, 0);
    atomic_store(&vmm_map_calls, 0); atomic_store(&vmm_successful_maps, 0);
    atomic_store(&lock_hold_total, 0); atomic_store(&lock_hold_max, 0);
    atomic_store(&lock_hold_count, 0);
    lock_metrics_enabled = locks;
    fixture_interrupts_enabled = true;
}
static void check_external(void) {
    uintptr_t expected = (uintptr_t)kernel_heap.start;
    usize maximum = fake_heap_capacity / sizeof(heap_block_t) + 1U;
    usize count = 0;
    for (heap_block_t *b = kernel_heap.first; b; b = b->next) {
        assert(++count <= maximum);
        assert((uintptr_t)b == expected && !(expected & 15U));
        assert(b->size && !(b->size & 15U));
        expected += sizeof(*b) + b->size;
    }
    assert(expected == (uintptr_t)kernel_heap.end);
}
static void emit_state(const char *label) {
    u64 free_payload = 0, used_payload = 0, largest = 0;
    u64 blocks = 0, free_blocks = 0, free_blocks_512_1007 = 0;
    u64 held_total = atomic_load(&lock_hold_total);
    u64 held_max = atomic_load(&lock_hold_max);
    u64 held_count = atomic_load(&lock_hold_count);
    uintptr_t expected = (uintptr_t)kernel_heap.start;
    usize max = fake_heap_capacity / sizeof(heap_block_t) + 1U;
    for (heap_block_t *b = kernel_heap.first; b; b = b->next) {
        assert(++blocks <= max && (uintptr_t)b == expected);
        expected += sizeof(*b) + b->size;
        if (b->free) {
            free_blocks++; free_payload += b->size;
            if (b->size >= 512U && b->size <= 1007U) free_blocks_512_1007++;
            if (b->size > largest) largest = b->size;
        } else used_payload += b->size;
    }
    assert(expected == (uintptr_t)kernel_heap.end);
#if INDEXED
    assert(heap_get_total_size() == used_payload + free_payload);
    assert(heap_get_used_size() == used_payload);
    assert(heap_get_free_size() == free_payload);
#endif
    u64 metadata = blocks * sizeof(heap_block_t);
    u64 requested = atomic_load(&live_requested);
    u64 waste = used_payload >= requested ? used_payload-requested : 0;
    u64 external_milli = free_payload ?
        ((free_payload-largest)*1000U)/free_payload : 0;
    printf("{\"type\":\"state\",\"label\":\"%s\",\"indexed\":%d,"
           "\"header_bytes\":%zu,\"mapped_bytes\":%llu,"
           "\"reserved_virtual_bytes\":%zu,\"blocks\":%llu,"
           "\"free_blocks\":%llu,\"live_requested\":%llu,"
           "\"free_blocks_512_1007\":%llu,"
           "\"used_payload\":%llu,\"free_payload\":%llu,"
           "\"largest_free_payload\":%llu,\"metadata_bytes\":%llu,"
           "\"internal_slack\":%llu,\"external_fragmentation_milli\":%llu,"
           "\"pmm_alloc_calls\":%llu,\"pmm_free_calls\":%llu,"
           "\"vmm_map_calls\":%llu,\"mapped_pages\":%llu,"
           "\"heap_lock_hold_total_ns\":%llu,\"heap_lock_hold_max_ns\":%llu,"
           "\"heap_lock_acquisitions\":%llu}\n",
           label, INDEXED, sizeof(heap_block_t),
           (unsigned long long)((u8 *)kernel_heap.end-(u8 *)kernel_heap.start),
           fake_heap_capacity, (unsigned long long)blocks,
           (unsigned long long)free_blocks,
           (unsigned long long)atomic_load(&live_requested),
           (unsigned long long)free_blocks_512_1007,
           (unsigned long long)used_payload, (unsigned long long)free_payload,
           (unsigned long long)largest, (unsigned long long)metadata,
           (unsigned long long)waste, (unsigned long long)external_milli,
           (unsigned long long)atomic_load(&pmm_alloc_calls),
           (unsigned long long)atomic_load(&pmm_free_calls),
           (unsigned long long)atomic_load(&vmm_map_calls),
           (unsigned long long)(((uintptr_t)kernel_heap.end-
               (uintptr_t)kernel_heap.start)/PAGE_SIZE),
           (unsigned long long)held_total, (unsigned long long)held_max,
           (unsigned long long)held_count);
}
static void sample_line(const char *scenario, const char *kind, u32 index,
                        u64 elapsed) {
    printf("{\"type\":\"sample\",\"scenario\":\"%s\","
           "\"kind\":\"%s\",\"index\":%u,\"ns\":%llu}\n",
           scenario, kind, index, (unsigned long long)elapsed);
}
static void timed_pair(const char *scenario, const char *tag, u32 index,
                       usize size) {
#if WORK_COUNTER
    bench_search_visits = 0;
#endif
    u64 started = tsc_ticks();
    void *ptr = kmalloc(size);
    u64 elapsed = tsc_ticks_to_ns(tsc_ticks() - started);
    assert(ptr && !((uintptr_t)ptr & 15U));
#if WORK_COUNTER
    printf("{\"type\":\"work\",\"scenario\":\"%s\","
           "\"kind\":\"%s\",\"index\":%u,\"steps\":%llu}\n",
           scenario,tag,index,(unsigned long long)bench_search_visits);
#endif
    live_requested += size;
    sample_line(scenario, tag, index, elapsed);
    started = tsc_ticks();
    kfree(ptr);
    elapsed = tsc_ticks_to_ns(tsc_ticks() - started);
    live_requested -= size;
    sample_line(scenario, "free", index, elapsed);
}
static u32 random_next(u32 *state) {
    u32 x = *state; x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x; return x;
}
static void warm_heap(void) {
    for (u32 i=0;i<64U;i++) { void *p=kmalloc(128U); assert(p); kfree(p); }
}
static void run_fragmented(void) {
    enum { BLOCKS=4096U };
    void *p[BLOCKS];
    fixture_init(64U*1024U*1024U,64U*1024U*1024U,LOCK_METRICS!=0); warm_heap();
    for (u32 i=0;i<BLOCKS;i++) { p[i]=kmalloc(16U); assert(p[i]); live_requested+=16U; }
    for (u32 i=0;i<BLOCKS;i+=2U) { kfree(p[i]); live_requested-=16U; }
    check_external(); emit_state("fragmented-before");
    atomic_store(&lock_hold_total,0); atomic_store(&lock_hold_max,0); atomic_store(&lock_hold_count,0);
    for (u32 i=0;i<SAMPLES;i++) timed_pair("fragmented","alloc512",i,512U);
    emit_state("fragmented-after");
}
static void run_same_bin(void) {
    enum { PAIRS=512U };
    void *candidate[PAIRS], *guard[PAIRS];
    fixture_init(32U*1024U*1024U,32U*1024U*1024U,LOCK_METRICS!=0); warm_heap();
    for (u32 i=0;i<PAIRS;i++) {
        candidate[i]=kmalloc(512U); guard[i]=kmalloc(16U);
        assert(candidate[i] && guard[i]); live_requested+=528U;
    }
    for (u32 i=0;i<PAIRS;i++) { kfree(candidate[i]); live_requested-=512U; }
    check_external(); emit_state("same-bin-before");
    atomic_store(&lock_hold_total,0); atomic_store(&lock_hold_max,0); atomic_store(&lock_hold_count,0);
    for (u32 i=0;i<SAMPLES;i++) timed_pair("same-bin","alloc1008",i,1008U);
    emit_state("same-bin-after");
}
static void run_mixed(u32 seed) {
    enum { ANCHORS=192U };
    static const usize anchor_sizes[]={64U,128U,256U,512U,4096U,65536U};
    static const usize churn_sizes[]={32U,64U,96U,128U,192U,256U,384U,512U,768U,1024U,4096U,65536U};
    void *anchor[ANCHORS]; usize sizes[ANCHORS];
    fixture_init(32U*1024U*1024U,32U*1024U*1024U,LOCK_METRICS!=0); warm_heap();
    for (u32 i=0;i<ANCHORS;i++) {
        sizes[i]=anchor_sizes[i%(sizeof(anchor_sizes)/sizeof(anchor_sizes[0]))];
        anchor[i]=kmalloc(sizes[i]); assert(anchor[i]); live_requested+=sizes[i];
    }
    for (u32 i=0;i<ANCHORS;i+=3U) { kfree(anchor[i]); live_requested-=sizes[i]; anchor[i]=NULL; }
    check_external(); emit_state("mixed-before");
    atomic_store(&lock_hold_total,0); atomic_store(&lock_hold_max,0); atomic_store(&lock_hold_count,0);
    for (u32 i=0;i<SAMPLES;i++) {
        usize size=churn_sizes[random_next(&seed)%(sizeof(churn_sizes)/sizeof(churn_sizes[0]))];
        timed_pair("mixed","alloc-mixed",i,size);
    }
    for (u32 i=0;i<ANCHORS;i++) if (anchor[i]) { kfree(anchor[i]); live_requested-=sizes[i]; }
    emit_state("mixed-after");
}
static void run_large(void) {
    static const usize sizes[]={4096U,32768U,65536U};
    fixture_init(32U*1024U*1024U,32U*1024U*1024U,LOCK_METRICS!=0); warm_heap();
    for (u32 s=0;s<3U;s++) {
        char tag[32]; snprintf(tag,sizeof(tag),"alloc-%zu",sizes[s]);
        for (u32 i=0;i<SAMPLES;i++) timed_pair("large",tag,i,sizes[s]);
    }
    emit_state("large-after");
}
static void run_boundaries(void) {
    static const usize sizes[]={16U,32U,48U,64U,80U,128U,240U,256U,272U,
        496U,512U,528U,992U,1008U,1024U,1040U,2032U,2048U,2064U,4096U};
    fixture_init(8U*1024U*1024U,8U*1024U*1024U,LOCK_METRICS!=0); warm_heap();
    for (u32 s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++) {
        char tag[32]; snprintf(tag,sizeof(tag),"alloc-%zu",sizes[s]);
        for (u32 i=0;i<SAMPLES;i++) timed_pair("boundaries",tag,i,sizes[s]);
    }
    emit_state("boundaries-after");
}
static void run_growth(void) {
    fixture_init(2U*1024U*1024U,HEAP_INITIAL_PAGES*PAGE_SIZE,LOCK_METRICS!=0);
    timed_pair("growth","alloc64k-first-growth",0,65536U);
    emit_state("growth-after");
}
static void run_pressure(void) {
    enum { CAP=256U*1024U, MAX=1024U };
    void *p[MAX]; u32 count=0;
    fixture_init(CAP,CAP,LOCK_METRICS!=0); warm_heap();
    while (count<MAX) {
        p[count]=kmalloc(1024U);
        if (!p[count]) break;
        live_requested+=1024U; count++;
    }
    assert(count>32U && count<MAX);
    for (u32 i=0;i<count;i+=2U) { kfree(p[i]); live_requested-=1024U; p[i]=NULL; }
    check_external(); emit_state("pressure-fragmented");
    u64 started=tsc_ticks(); void *failed=kmalloc(4096U);
    u64 elapsed=tsc_ticks_to_ns(tsc_ticks()-started);
    assert(!failed); sample_line("pressure","fail4096",0,elapsed);
    for (u32 i=1;i<count;i+=2U) { kfree(p[i]); live_requested-=1024U; }
    void *recovered=kmalloc(4096U); assert(recovered); live_requested+=4096U;
    kfree(recovered); live_requested-=4096U;
    emit_state("pressure-recovered");
}
typedef struct { u32 seed; pthread_barrier_t *barrier; int cpu; } worker_arg_t;
static _Atomic u64 concurrent_failures;
static void *concurrent_worker(void *opaque) {
    worker_arg_t *arg=opaque; void *slots[24]={0}; usize sizes[24]={0}; u8 values[24]={0};
    cpu_set_t worker_set; CPU_ZERO(&worker_set); CPU_SET(arg->cpu,&worker_set);
    assert(pthread_setaffinity_np(pthread_self(),sizeof(worker_set),&worker_set)==0);
    u32 seed=arg->seed; int br=pthread_barrier_wait(arg->barrier);
    assert(br==0 || br==PTHREAD_BARRIER_SERIAL_THREAD);
    for (u32 i=0;i<2400U;i++) {
        u32 slot=random_next(&seed)%24U;
        if (slots[slot]) {
            u8 *p=slots[slot]; if (p[0]!=values[slot] || p[sizes[slot]-1U]!=values[slot])
                atomic_fetch_add(&concurrent_failures,1U);
            kfree(slots[slot]); live_requested-=sizes[slot]; slots[slot]=NULL;
        } else {
            usize size=(random_next(&seed)%128U+1U)*16U;
            slots[slot]=kmalloc(size);
            if (!slots[slot]) { atomic_fetch_add(&concurrent_failures,1U); continue; }
            sizes[slot]=size; live_requested+=size;
            values[slot]=(u8)(arg->seed+slot+1U); memset(slots[slot],values[slot],size);
        }
    }
    for (u32 i=0;i<24U;i++) if (slots[i]) { kfree(slots[i]); live_requested-=sizes[i]; }
    return NULL;
}
static void run_concurrent(u32 seed) {
    enum { WORKERS=4U }; pthread_t threads[WORKERS]; worker_arg_t args[WORKERS];
    pthread_barrier_t barrier; assert(pthread_barrier_init(&barrier,NULL,WORKERS+1U)==0);
    fixture_init(16U*1024U*1024U,16U*1024U*1024U,LOCK_METRICS!=0); warm_heap();
    atomic_store(&lock_hold_total,0); atomic_store(&lock_hold_max,0); atomic_store(&lock_hold_count,0);
    atomic_store(&concurrent_failures,0);
    for (u32 i=0;i<WORKERS;i++) { args[i]=(worker_arg_t){seed+i*0x9e3779b9U,&barrier,
        fixture_cpus[i%fixture_cpu_count]};
        assert(pthread_create(&threads[i],NULL,concurrent_worker,&args[i])==0); }
    u64 started=clock_ns(); int br=pthread_barrier_wait(&barrier); assert(br==0 || br==PTHREAD_BARRIER_SERIAL_THREAD);
    for (u32 i=0;i<WORKERS;i++) assert(pthread_join(threads[i],NULL)==0);
    u64 elapsed=clock_ns()-started;
    assert(atomic_load(&concurrent_failures)==0 && atomic_load(&live_requested)==0);
    printf("{\"type\":\"concurrent\",\"elapsed_ns\":%llu,\"operations\":%u,\"failures\":%llu}\n",
        (unsigned long long)elapsed,WORKERS*2400U,
        (unsigned long long)atomic_load(&concurrent_failures));
    assert(pthread_barrier_destroy(&barrier)==0); check_external(); emit_state("concurrent-after");
}
static void emit_timer_overhead(void) {
    u64 values[1000];
    for (u32 i=0;i<1000U;i++) {
        u64 start=tsc_ticks(); values[i]=tsc_ticks_to_ns(tsc_ticks()-start);
    }
    qsort(values,1000U,sizeof(values[0]),compare_u64);
    printf("{\"type\":\"timer\",\"median_pair_ns\":%llu}\n",
           (unsigned long long)values[500]);
}
static void pin_measurement_threads(void) {
    cpu_set_t allowed; CPU_ZERO(&allowed);
    assert(sched_getaffinity(0,sizeof(allowed),&allowed)==0);
    for (int cpu=0;cpu<CPU_SETSIZE;cpu++)
        if (CPU_ISSET(cpu,&allowed)) fixture_cpus[fixture_cpu_count++]=cpu;
    assert(fixture_cpu_count>0);
    cpu_set_t primary; CPU_ZERO(&primary); CPU_SET(fixture_cpus[0],&primary);
    assert(sched_setaffinity(0,sizeof(primary),&primary)==0);
}
int main(int argc,char **argv) {
    if (argc!=4) return 2;
    u32 trial=(u32)strtoul(argv[1],NULL,10), seed=(u32)strtoul(argv[3],NULL,10);
    (void)trial;
    pin_measurement_threads();
    calibrate_tsc();
    emit_timer_overhead();
    if (!strcmp(argv[2],"fragmented")) run_fragmented();
    else if (!strcmp(argv[2],"same-bin")) run_same_bin();
    else if (!strcmp(argv[2],"mixed")) run_mixed(seed);
    else if (!strcmp(argv[2],"large")) run_large();
    else if (!strcmp(argv[2],"boundaries")) run_boundaries();
    else if (!strcmp(argv[2],"growth")) run_growth();
    else if (!strcmp(argv[2],"pressure")) run_pressure();
    else if (!strcmp(argv[2],"concurrent")) run_concurrent(seed);
    else return 2;
    return 0;
}
'''


def quantile(values: list[int], fraction: float) -> int:
    """Nearest-rank percentile (1-based), with a minimum rank of one."""
    if not values or not 0.0 < fraction <= 1.0:
        raise ValueError("sample set or percentile is invalid")
    ordered = sorted(values)
    rank = max(1, math.ceil(len(ordered) * fraction))
    return ordered[min(rank - 1, len(ordered) - 1)]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def get_source(revision: str, path: str) -> bytes:
    if revision == CURRENT:
        return (ROOT / path).read_bytes()
    return subprocess.run(["git", "show", f"{revision}:{path}"], cwd=ROOT,
                          check=True, stdout=subprocess.PIPE).stdout


def self_test() -> None:
    assert quantile([9, 1, 5, 3, 7], 0.50) == 5
    assert quantile([9, 1, 5, 3, 7], 0.95) == 9
    assert quantile([9, 1, 5, 3, 7], 0.99) == 9
    try:
        quantile([], 0.5)
    except ValueError:
        pass
    else:
        raise AssertionError("empty measurement set was accepted")
    try:
        quantile([1], 0.0)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid percentile was accepted")
    control = [10, 20, 30, 40, 50]
    assert quantile([value + 1000 for value in control], 0.50) == \
           quantile(control, 0.50) + 1000
    assert BASELINE.startswith("3288152")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="new ignored build/ artifact directory")
    parser.add_argument("--cc", default=os.environ.get("HOST_CC", "gcc"))
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--samples", type=int, default=256)
    parser.add_argument("--scenarios", nargs="+", choices=("fragmented", "same-bin",
        "mixed", "large", "boundaries", "growth", "pressure", "concurrent"),
        default=("fragmented", "same-bin", "mixed", "large", "boundaries", "growth", "pressure", "concurrent"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("heap benchmark measurement controls passed")
        return 0
    if not args.output:
        parser.error("--output is required unless --self-test is selected")
    if args.trials < 5:
        parser.error("at least five independent trials are required")
    if not 32 <= args.samples <= 4096:
        parser.error("--samples must be between 32 and 4096")
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("serialized RDTSCP measurements currently require Linux x86-64")
    cpu_flags: set[str] = set()
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("flags") or line.startswith("Features"):
                cpu_flags.update(line.partition(":")[2].split())
                break
    except OSError as error:
        parser.error(f"cannot verify host TSC capabilities: {error}")
    if not {"rdtscp", "constant_tsc"}.issubset(cpu_flags) or not (
            {"nonstop_tsc", "invariant_tsc"} & cpu_flags):
        parser.error("host must report RDTSCP, constant TSC, and nonstop/invariant TSC")
    output = args.output if args.output.is_absolute() else ROOT / args.output
    output = output.resolve()
    if not output.is_relative_to((ROOT / "build").resolve()):
        parser.error("benchmark artifacts must be stored under ignored build/")
    if output.exists():
        parser.error(f"refusing to overwrite existing evidence directory: {output}")
    output.mkdir(parents=True)
    revisions = [("first-fit", BASELINE, False), ("indexed", CURRENT, True)]
    inputs: dict[str, dict[str, bytes]] = {}
    for label, revision, _ in revisions:
        inputs[label] = {name: get_source(revision, f"kernel/{name}") for name in
                         ("src/heap.c", "include/heap.h")}
        (output / f"{label}-heap.c").write_bytes(inputs[label]["src/heap.c"])
        (output / f"{label}-heap.h").write_bytes(inputs[label]["include/heap.h"])
    host = subprocess.run([args.cc, "--version"], check=True, text=True,
                          stdout=subprocess.PIPE).stdout.splitlines()[0]
    allowed = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    if hasattr(os, "sched_setaffinity") and len(allowed) > 1:
        single_cpus = [allowed[0]]
        multi_cpus = allowed[:2]
    else:
        single_cpus = allowed
        multi_cpus = allowed
    flags = ["-std=gnu11", "-O0", "-g0", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-pthread"]
    metadata = {
        "baseline_revision": BASELINE,
        "current_revision": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
            check=True, text=True, stdout=subprocess.PIPE).stdout.strip(),
        "working_tree_source": "read from kernel/src/heap.c and kernel/include/heap.h",
        "source_sha256": {label: {name: sha256(data) for name, data in files.items()}
                          for label, files in inputs.items()},
        "block_header_bytes": {"first-fit": 32, "indexed": 48},
        "compiler": host, "compile_flags": flags,
        "host": {"system": platform.platform(), "machine": platform.machine(),
                 "tsc_flags": sorted(cpu_flags & {"rdtscp", "constant_tsc", "nonstop_tsc", "invariant_tsc"}),
                 "cpu_affinity_available": bool(allowed),
                 "single_thread_cpus": single_cpus,
                 "concurrent_cpus": multi_cpus},
        "timer": "serialized RDTSCP deltas calibrated against CLOCK_MONOTONIC_RAW over 100 ms; timer overhead is retained, not subtracted; measurement threads are pinned to one CPU except explicitly pinned concurrent workers",
        "lock_metrics": "separate concurrent run; RDTSCP reads and atomics inside the pthread lock adapter; workers are pinned to CPUs; not native spinlock or IRQ timing",
        "measurement": "unoptimized host extraction, same pthread/PMM/VMM adapter; no sanitizers or full verifier",
        "trials": args.trials, "samples_per_size_per_scenario_trial": args.samples,
        "seed_formula": "0x57a4e21b XOR (trial * 0x9e3779b9) XOR scenario-index",
        "scenarios": args.scenarios,
        "script_sha256": sha256(Path(__file__).read_bytes()),
        "invocation": ["python3", str(Path(__file__).relative_to(ROOT)), *sys.argv[1:]],
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    binaries: dict[str, Path] = {}
    executed_commands = []
    for label, revision, indexed in revisions:
        raw_production = production_source(inputs[label]["src/heap.c"].decode(), indexed)
        c_source = (COMMON_C.replace("#if INDEXED", f"#if {int(indexed)}") +
                    raw_production +
                    HARNESS_C)
        source_path = output / f"{label}-benchmark.c"
        source_path.write_text(c_source)
        binary = output / f"{label}-benchmark"
        command = [args.cc, *flags, f"-DINDEXED={int(indexed)}",
                   "-DWORK_COUNTER=0", f"-DSAMPLES={args.samples}",
                   str(source_path), "-o", str(binary)]
        executed_commands.append({"kind": "compile", "revision": label,
                                  "command": command})
        subprocess.run(command, cwd=ROOT, check=True)
        binaries[label] = binary
        if "concurrent" in args.scenarios:
            lock_binary = output / f"{label}-lock-metrics"
            command = [args.cc, *flags, f"-DINDEXED={int(indexed)}",
                "-DWORK_COUNTER=0", "-DLOCK_METRICS=1", f"-DSAMPLES={args.samples}",
                str(source_path), "-o", str(lock_binary)]
            executed_commands.append({"kind": "compile-lock-metrics", "revision": label,
                                      "command": command})
            subprocess.run(command, cwd=ROOT, check=True)
            binaries[f"{label}-locks"] = lock_binary
        if "fragmented" in args.scenarios or "same-bin" in args.scenarios:
            work_source = (COMMON_C.replace("#if INDEXED", f"#if {int(indexed)}") +
                           instrument_search(raw_production, indexed) + HARNESS_C)
            work_source_path = output / f"{label}-work-probe.c"
            work_source_path.write_text(work_source)
            work_binary = output / f"{label}-work-probe"
            command = [args.cc, *flags, f"-DINDEXED={int(indexed)}",
                "-DWORK_COUNTER=1", f"-DSAMPLES={args.samples}",
                str(work_source_path), "-o", str(work_binary)]
            executed_commands.append({"kind": "compile-work-counter", "revision": label,
                                      "command": command})
            subprocess.run(command, cwd=ROOT, check=True)
            binaries[f"{label}-work"] = work_binary
    scenarios = metadata["scenarios"]
    raw_path = output / "raw.jsonl"
    records: list[dict] = []
    run_index = 0
    work_records: list[dict] = []
    lock_records: list[dict] = []
    with raw_path.open("w") as raw:
        for trial in range(args.trials):
            for scenario_index, scenario in enumerate(scenarios):
                order = revisions if (trial + scenario_index) % 2 == 0 else list(reversed(revisions))
                for label, _, _ in order:
                    seed = 0x57A4E21B ^ (trial * 0x9E3779B9) ^ scenario_index
                    cpu_set = multi_cpus if scenario == "concurrent" else single_cpus
                    preexec = None
                    if hasattr(os, "sched_setaffinity") and cpu_set:
                        preexec = lambda cpus=cpu_set: os.sched_setaffinity(0, set(cpus))
                    command = [str(binaries[label]), str(trial), scenario, str(seed)]
                    executed_commands.append({"kind": "benchmark", "revision": label,
                        "scenario": scenario, "trial": trial, "seed": seed,
                        "affinity": cpu_set, "command": command})
                    run = subprocess.run(command, cwd=ROOT, text=True,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         preexec_fn=preexec, check=True)
                    for line in run.stdout.splitlines():
                        obj = json.loads(line)
                        obj.update({"revision": label, "trial": trial,
                                    "scenario": scenario, "seed": seed,
                                    "run_index": run_index})
                        records.append(obj)
                        raw.write(json.dumps(obj, separators=(",", ":")) + "\n")
                    if run.stderr:
                        (output / f"stderr-{run_index:04d}.txt").write_text(run.stderr)
                    run_index += 1
        for scenario in ("fragmented", "same-bin"):
            if scenario not in scenarios:
                continue
            for label, _, _ in revisions:
                work_affinity = None
                if hasattr(os, "sched_setaffinity") and single_cpus:
                    work_affinity = lambda cpus=single_cpus: os.sched_setaffinity(0,set(cpus))
                seed = 0x57A4E21B
                command = [str(binaries[f"{label}-work"]), "0", scenario, str(seed)]
                executed_commands.append({"kind": "work-counter", "revision": label,
                    "scenario": scenario, "seed": seed, "affinity": single_cpus,
                    "command": command})
                run = subprocess.run(command, cwd=ROOT, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    preexec_fn=work_affinity, check=True)
                for line in run.stdout.splitlines():
                    obj = json.loads(line)
                    if obj["type"] == "work":
                        obj.update({"revision": label, "scenario": scenario})
                        work_records.append(obj)
                if run.stderr:
                    (output / f"work-stderr-{scenario}-{label}.txt").write_text(run.stderr)
        if "concurrent" in scenarios:
            lock_raw = (output / "lock-metrics.jsonl").open("w")
            try:
                for trial in range(args.trials):
                    for label, _, _ in revisions:
                        affinity = None
                        if hasattr(os,"sched_setaffinity") and multi_cpus:
                            affinity = lambda cpus=multi_cpus: os.sched_setaffinity(0,set(cpus))
                        seed = 0x57A4E21B ^ (trial * 0x9E3779B9) ^ scenarios.index("concurrent")
                        command = [str(binaries[f"{label}-locks"]), str(trial),
                                   "concurrent", str(seed)]
                        executed_commands.append({"kind": "lock-metrics", "revision": label,
                            "trial": trial, "seed": seed, "affinity": multi_cpus,
                            "command": command})
                        run = subprocess.run(command, cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            preexec_fn=affinity, check=True)
                        for line in run.stdout.splitlines():
                            obj=json.loads(line)
                            if obj["type"] in ("concurrent","state"):
                                obj.update({"revision":label,"trial":trial,"seed":seed})
                                if obj["type"]=="state" and obj["label"]!="concurrent-after":
                                    continue
                                lock_records.append(obj)
                                lock_raw.write(json.dumps(obj,separators=(",",":"))+"\n")
            finally:
                lock_raw.close()
    (output / "commands.json").write_text(json.dumps(executed_commands, indent=2) + "\n")
    work_controls = []
    if work_records:
        by_case: dict[tuple[str, str], list[int]] = {}
        for obj in work_records:
            by_case.setdefault((obj["scenario"], obj["revision"]), []).append(obj["steps"])
        for scenario in ("fragmented", "same-bin"):
            if scenario not in scenarios:
                continue
            baseline_steps = by_case[(scenario, "first-fit")]
            indexed_steps = by_case[(scenario, "indexed")]
            assert len(baseline_steps) == len(indexed_steps) == args.samples
            assert min(baseline_steps) > max(indexed_steps)
            if scenario == "fragmented":
                assert min(baseline_steps) >= 4096 and max(indexed_steps) <= 2
            work_controls.append({"scenario": scenario,
                "first_fit_steps_median": int(statistics.median(baseline_steps)),
                "indexed_steps_median": int(statistics.median(indexed_steps)),
                "samples": len(baseline_steps)})
        with (output / "work-probe.jsonl").open("w") as raw_work:
            for obj in work_records:
                raw_work.write(json.dumps(obj, separators=(",", ":")) + "\n")
        (output / "work-probe.json").write_text(json.dumps(work_controls, indent=2) + "\n")
    sample_groups: dict[tuple[str, str, str], list[int]] = {}
    per_trial: dict[tuple[str, str, str, int], list[int]] = {}
    concurrent: dict[tuple[str, int], int] = {}
    states = []
    for obj in records:
        if obj["type"] == "sample":
            key = (obj["revision"], obj["scenario"], obj["kind"])
            sample_groups.setdefault(key, []).append(obj["ns"])
            per_trial.setdefault((*key, obj["trial"]), []).append(obj["ns"])
        elif obj["type"] == "concurrent":
            concurrent[(obj["revision"], obj["trial"])] = obj["elapsed_ns"]
        elif obj["type"] == "state":
            states.append(obj)
    summaries = []
    for (revision, scenario, kind), values in sorted(sample_groups.items()):
        trial_values = [v for (r, s, k, _), v in per_trial.items()
                        if (r, s, k) == (revision, scenario, kind)]
        trial_medians = [int(statistics.median(v)) for v in trial_values]
        trial_quantiles = [{f"p{int(q * 100)}": quantile(v, q)
                            for q in (0.50, 0.95, 0.99)} for v in trial_values]
        trial_percentile_medians = {
            key: int(statistics.median([row[key] for row in trial_quantiles]))
            for key in ("p50", "p95", "p99")}
        trial_percentile_ranges = {
            key: [min(row[key] for row in trial_quantiles),
                  max(row[key] for row in trial_quantiles)]
            for key in ("p50", "p95", "p99")}
        summaries.append({"revision": revision, "scenario": scenario, "operation": kind,
            "samples": len(values), "pooled_p50_ns": quantile(values,0.50),
            "pooled_p95_ns": quantile(values,0.95), "pooled_p99_ns": quantile(values,0.99),
            "pooled_max_ns": max(values), "trial_median_min_ns": min(trial_medians),
            "trial_median_median_ns": int(statistics.median(trial_medians)),
            "trial_median_max_ns": max(trial_medians),
            "median_of_trial_percentiles_ns": trial_percentile_medians,
            "trial_percentile_ranges_ns": trial_percentile_ranges})
    concurrent_summary = []
    for revision in ("first-fit", "indexed"):
        values = [ns for (r, _), ns in concurrent.items() if r == revision]
        if values:
            concurrent_summary.append({"revision": revision, "trials": len(values),
                "elapsed_median_ns": int(statistics.median(values)),
                "operations_per_trial": 4*2400,
                "throughput_median_ops_s": int((4*2400*1_000_000_000)/statistics.median(values)),
                "elapsed_min_ns": min(values), "elapsed_max_ns": max(values)})
    lock_summary = []
    for revision in ("first-fit","indexed"):
        lock_states=[x for x in lock_records if x["type"]=="state" and x["revision"]==revision]
        if lock_states:
            means=[x["heap_lock_hold_total_ns"]/max(1,x["heap_lock_acquisitions"]) for x in lock_states]
            maxima=[x["heap_lock_hold_max_ns"] for x in lock_states]
            lock_summary.append({"revision":revision,"trials":len(lock_states),
                "mean_hold_median_ns":int(statistics.median(means)),
                "max_hold_median_ns":int(statistics.median(maxima)),
                "instrumentation":"RDTSCP reads and atomic counters inside pthread lock adapter"})
    timer_summary = []
    for revision in ("first-fit", "indexed"):
        values = [obj["median_pair_ns"] for obj in records
                  if obj["type"] == "timer" and obj["revision"] == revision]
        if values:
            timer_summary.append({"revision": revision, "runs": len(values),
                                  "median_pair_ns_median": int(statistics.median(values)),
                                  "median_pair_ns_min": min(values),
                                  "median_pair_ns_max": max(values)})
    (output / "summary.json").write_text(json.dumps({
        "latency": summaries, "timer_overhead": timer_summary,
        "concurrent": concurrent_summary,
        "lock_metrics":lock_summary, "states": states
    }, indent=2) + "\n")
    print(f"raw measurements: {raw_path}")
    print(f"summary: {output / 'summary.json'}")
    print(f"sources and generated binaries: {output}")
    for item in summaries:
        print("{revision:8s} {scenario:12s} {operation:20s} n={samples:4d} "
              "trial-median p50={p50:8d} p95={p95:8d} p99={p99:8d} ns "
              "(pooled max={pooled_max_ns} ns)".format(
                  **item, **item["median_of_trial_percentiles_ns"]))
    for item in concurrent_summary:
        print("{revision:8s} concurrent {throughput_median_ops_s:8d} ops/s "
              "({elapsed_median_ns} ns median)".format(**item))
    for item in lock_summary:
        print("{revision:8s} lock adapter mean/max hold {mean_hold_median_ns}/"
              "{max_hold_median_ns} ns median (instrumented)".format(**item))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
