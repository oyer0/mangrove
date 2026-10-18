#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production heap functions with host PMM/VMM and lock adapters."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


def function(path, name):
    source = (ROOT / path).read_text()
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
    raise AssertionError(f"definition not found: {name}")


COMMON = r"""
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
typedef size_t usize; typedef uint64_t phys_addr_t;
typedef struct { pthread_mutex_t native; } spinlock_t;
typedef struct { pthread_mutex_t native; } mutex_t;
typedef struct { int unused; } page_table_t;
typedef struct heap_block {
    usize size; bool free, in_free_bin;
    struct heap_block *next, *prev, *free_next, *free_prev;
} heap_block_t;
typedef struct { void *start, *end; heap_block_t *first, *last; } heap_t;
static heap_t kernel_heap;
static spinlock_t heap_lock;
static mutex_t heap_growth_lock;
#define HEAP_FREE_BIN_COUNT 32U
static heap_block_t *heap_free_bins[HEAP_FREE_BIN_COUNT];
static u32 heap_free_bin_bitmap;
static u64 heap_total_payload, heap_used_payload, heap_free_payload;
static uintptr_t fake_heap_base;
static usize fake_heap_capacity;
#define HEAP_START fake_heap_base
#define HEAP_LIMIT (fake_heap_base + fake_heap_capacity)
#define HEAP_INITIAL_PAGES 4U
#define PAGE_SIZE 4096U
#define PTE_PRESENT 1U
#define PTE_READWRITE 2U
#define HEAP_VERIFY 1
static page_table_t fake_pml4;
static _Atomic u32 pmm_alloc_calls, pmm_free_calls, vmm_map_calls;
static _Atomic int pmm_fail_at, vmm_fail_at;
static _Atomic u64 heap_lock_total_ns, heap_lock_max_ns;
static bool panic_child_exit;
static _Thread_local bool fixture_interrupts_enabled = true;
static _Thread_local u64 fixture_lock_started_ns;
static bool heap_header_valid(const heap_block_t *block);
static bool heap_block_valid(const heap_block_t *block);
static void heap_free_insert_locked(heap_block_t *block);
static bool heap_verify(const char *where);
static u64 monotonic_ns(void) {
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (u64)time.tv_sec * 1000000000ULL + (u64)time.tv_nsec;
}
static u64 spin_lock_irqsave(spinlock_t *lock) {
    bool was_enabled = fixture_interrupts_enabled;
    fixture_interrupts_enabled = false;
    assert(pthread_mutex_lock(&lock->native) == 0);
    fixture_lock_started_ns = monotonic_ns();
    return was_enabled ? (1ULL << 9) : 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    u64 elapsed = monotonic_ns() - fixture_lock_started_ns;
    if (lock == &heap_lock) {
        atomic_fetch_add(&heap_lock_total_ns, elapsed);
        u64 prior = atomic_load(&heap_lock_max_ns);
        while (elapsed > prior && !atomic_compare_exchange_weak(
                   &heap_lock_max_ns, &prior, elapsed)) { }
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
static u64 pmm_alloc_frame(void) {
    u32 call = atomic_fetch_add(&pmm_alloc_calls, 1U);
    if ((int)call == atomic_load(&pmm_fail_at)) return 0;
    return ((u64)call + 1U) * PAGE_SIZE;
}
static void pmm_free_frame(phys_addr_t frame) {
    assert(frame != 0);
    atomic_fetch_add(&pmm_free_calls, 1U);
}
static page_table_t *vmm_get_kernel_pml4(void) { return &fake_pml4; }
static bool vmm_map(page_table_t *pml4, void *address, phys_addr_t frame,
                    u64 flags) {
    assert(pml4 == &fake_pml4 && frame &&
           flags == (PTE_PRESENT | PTE_READWRITE));
    uintptr_t at = (uintptr_t)address;
    assert(at >= fake_heap_base && at + PAGE_SIZE <= HEAP_LIMIT &&
           (at & (PAGE_SIZE - 1U)) == 0);
    u32 call = atomic_fetch_add(&vmm_map_calls, 1U);
    return (int)call != atomic_load(&vmm_fail_at);
}
static void kprint(const char *format, ...) { (void)format; }
static void panic(const char *message) {
    kprint("heap panic: %s\n", message);
    if (panic_child_exit) _exit(86);
    fprintf(stderr, "unexpected heap panic: %s\n", message);
    abort();
}
"""


PRODUCTION_BASE = "".join(
    function("kernel/src/heap.c", name)
    for name in (
        "heap_bin_index",
        "heap_maximum_blocks_locked",
        "heap_header_valid",
        "heap_block_valid",
        "heap_free_insert_locked",
        "heap_free_remove_locked",
        "heap_free_find_locked",
        "heap_grow_reserved",
        "kmalloc",
        "kfree",
        "heap_get_total_size",
        "heap_get_used_size",
        "heap_get_free_size",
    )
)
PRODUCTION_VERIFY = function("kernel/src/heap.c", "heap_verify")
PRODUCTION_TEST = PRODUCTION_BASE + PRODUCTION_VERIFY
PRODUCTION_BENCH = PRODUCTION_BASE + r"""
static bool heap_verify(const char *where) { (void)where; return true; }
"""


HARNESS = r"""
static void fixture_init(usize capacity, usize initially_mapped) {
    void *memory = NULL;
    assert(posix_memalign(&memory, PAGE_SIZE, capacity) == 0 && memory);
    if (!initially_mapped)
        initially_mapped = HEAP_INITIAL_PAGES * PAGE_SIZE;
    assert(initially_mapped >= HEAP_INITIAL_PAGES * PAGE_SIZE &&
           initially_mapped <= capacity && !(initially_mapped & (PAGE_SIZE - 1U)));
    fake_heap_base = (uintptr_t)memory;
    fake_heap_capacity = capacity;
    assert(pthread_mutex_init(&heap_lock.native, NULL) == 0);
    assert(pthread_mutex_init(&heap_growth_lock.native, NULL) == 0);
    memset(heap_free_bins, 0, sizeof(heap_free_bins));
    heap_free_bin_bitmap = 0;
    heap_total_payload = heap_used_payload = heap_free_payload = 0;
    kernel_heap.start = memory;
    kernel_heap.end = (u8 *)memory + initially_mapped;
    kernel_heap.first = (heap_block_t *)memory;
    kernel_heap.last = kernel_heap.first;
    *kernel_heap.first = (heap_block_t){
        .size = initially_mapped - sizeof(heap_block_t),
        .free = true,
        .in_free_bin = false,
    };
    heap_total_payload = heap_free_payload = kernel_heap.first->size;
    heap_free_insert_locked(kernel_heap.first);
    atomic_store(&pmm_alloc_calls, 0U);
    atomic_store(&pmm_free_calls, 0U);
    atomic_store(&vmm_map_calls, 0U);
    atomic_store(&pmm_fail_at, -1);
    atomic_store(&vmm_fail_at, -1);
    atomic_store(&heap_lock_total_ns, 0U);
    atomic_store(&heap_lock_max_ns, 0U);
}

static void fixture_destroy(void) {
    void *memory = (void *)fake_heap_base;
    assert(pthread_mutex_destroy(&heap_growth_lock.native) == 0);
    assert(pthread_mutex_destroy(&heap_lock.native) == 0);
    free(memory);
    fake_heap_base = 0;
    fake_heap_capacity = 0;
}

static void independent_check(void) {
    heap_block_t *block = kernel_heap.first;
    heap_block_t *previous = NULL;
    uintptr_t expected = (uintptr_t)kernel_heap.start;
    usize maximum = fake_heap_capacity / sizeof(heap_block_t);
    usize visited = 0, free_count = 0, indexed_count = 0;
    u64 total = 0, used = 0, available = 0;

    while (block) {
        assert(++visited <= maximum);
        assert((uintptr_t)block == expected && ((uintptr_t)block & 15U) == 0);
        assert(block->size && !(block->size & 15U));
        assert(block->prev == previous);
        if (block->free) {
            assert(block->in_free_bin);
            assert(!previous || !previous->free);
        } else {
            assert(!block->in_free_bin && !block->free_next &&
                   !block->free_prev);
        }
        expected += sizeof(*block) + block->size;
        total += block->size;
        if (block->free) {
            heap_block_t *candidate = heap_free_bins[heap_bin_index(block->size)];
            bool found = false;
            usize steps = 0;
            free_count++;
            available += block->size;
            while (candidate) {
                assert(++steps <= maximum);
                if (candidate == block) { found = true; break; }
                candidate = candidate->free_next;
            }
            assert(found);
        } else used += block->size;
        if (block->next) assert(block->next->prev == block);
        else assert(block == kernel_heap.last);
        previous = block;
        block = block->next;
    }
    assert(expected == (uintptr_t)kernel_heap.end);
    assert(total + visited * sizeof(heap_block_t) ==
           (uintptr_t)kernel_heap.end - (uintptr_t)kernel_heap.start);
    for (u32 index = 0; index < HEAP_FREE_BIN_COUNT; index++) {
        heap_block_t *previous = NULL;
        heap_block_t *candidate = heap_free_bins[index];
        assert(((heap_free_bin_bitmap >> index) & 1U) == (candidate != NULL));
        while (candidate) {
            assert(++indexed_count <= maximum);
            assert(candidate->free && candidate->in_free_bin &&
                   candidate->free_prev == previous);
            assert(heap_bin_index(candidate->size) == index);
            previous = candidate;
            candidate = candidate->free_next;
        }
    }
    assert(free_count == indexed_count);
    if (total != heap_get_total_size())
        fprintf(stderr, "heap accounting total model=%llu counter=%llu used=%llu free=%llu\n",
                (unsigned long long)total,
                (unsigned long long)heap_get_total_size(),
                (unsigned long long)used,
                (unsigned long long)available);
    assert(total == heap_get_total_size());
    assert(used == heap_get_used_size());
    assert(available == heap_get_free_size());
    assert(total == used + available);
    assert(heap_verify("host fixture"));
}

static void expect_child_panic(void (*action)(void)) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        panic_child_exit = true;
        alarm(2U);
        action();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 86);
}

static void *death_pointer;
static void free_death_pointer(void) { kfree(death_pointer); }
static void expect_free_panic(void *pointer) {
    death_pointer = pointer;
    expect_child_panic(free_death_pointer);
}
static void free_interior(void) {
    kfree((u8 *)death_pointer + 16U);
}
static void free_misaligned(void) {
    kfree((u8 *)death_pointer + 1U);
}
static void free_outside(void) {
    kfree((void *)(fake_heap_base - 16U));
}
static void free_after_heap(void) {
    kfree((void *)(fake_heap_base + fake_heap_capacity + 16U));
}
static void panic_double_free(void) {
    void *pointer = kmalloc(64U);
    assert(pointer);
    kfree(pointer);
    kfree(pointer);
}

static void duplicate_singleton_insert(void) {
    heap_free_insert_locked(kernel_heap.first);
}
static void reciprocal_singleton_cycle_search(void) {
    void *small = kmalloc(512U), *barrier = kmalloc(512U);
    assert(small && barrier);
    kfree(small);
    heap_block_t *block = (heap_block_t *)((u8 *)small -
                                           sizeof(heap_block_t));
    /* A reciprocal self-cycle is the state the old duplicate insertion path
     * could create. This request shares its bin but cannot use the block. */
    block->free_next = block;
    block->free_prev = block;
    (void)heap_free_find_locked(528U);
}
static void broken_reciprocal_search(void) {
    void *small = kmalloc(512U), *barrier = kmalloc(512U);
    assert(small && barrier);
    kfree(small);
    heap_block_t *block = (heap_block_t *)((u8 *)small -
                                           sizeof(heap_block_t));
    block->free_next = block;
    (void)heap_free_find_locked(512U);
}
static void broken_multi_block_cycle_search(void) {
    void *a = kmalloc(512U), *guard_a = kmalloc(512U);
    void *b = kmalloc(512U), *guard_b = kmalloc(512U);
    assert(a && guard_a && b && guard_b);
    kfree(a);
    kfree(b);
    heap_block_t *head = heap_free_bins[heap_bin_index(512U)];
    heap_block_t *second = head->free_next;
    assert(second && second != head);
    second->free_next = head;
    head->free_prev = second;
    (void)heap_free_find_locked(528U);
}
static void broken_bitmap_search(void) {
    heap_free_bin_bitmap = 0;
    (void)heap_free_find_locked(16U);
}
static void broken_membership_search(void) {
    heap_free_bins[heap_bin_index(kernel_heap.first->size)]->in_free_bin = false;
    (void)heap_free_find_locked(16U);
}
static void invalid_bin_head_search(void) {
    u32 index = heap_bin_index(kernel_heap.first->size);
    heap_free_bins[index] = (heap_block_t *)(fake_heap_base - 16U);
    (void)heap_free_find_locked(16U);
}
static void allocated_index_corruption(void) {
    void *pointer = kmalloc(64U);
    assert(pointer);
    heap_block_t *block = (heap_block_t *)((u8 *)pointer -
                                           sizeof(heap_block_t));
    block->in_free_bin = true;
    block->free_next = heap_free_bins[heap_bin_index(kernel_heap.first->size)];
    (void)kmalloc(16U);
}
static void physical_chain_corruption(void) {
    kernel_heap.first->next = kernel_heap.first;
    (void)kmalloc(16U);
}
static void accounting_corruption(void) {
    heap_used_payload++;
    (void)kmalloc(16U);
}
static void header_size_corruption(void) {
    kernel_heap.first->size = (usize)-16U;
    (void)kmalloc(16U);
}

static void basic_and_failure_test(void) {
    fixture_init(8U * 1024U * 1024U, 0);
    void *a = kmalloc(17U), *b = kmalloc(100U), *c = kmalloc(4096U);
    assert(a && b && c && !((uintptr_t)a & 15U) &&
           !((uintptr_t)b & 15U) && !((uintptr_t)c & 15U));
    memset(a, 0x5a, 17U); memset(b, 0x6b, 100U); memset(c, 0x7c, 4096U);
    independent_check();
    kfree(b); kfree(a); kfree(c);
    kfree(NULL);
    independent_check();
    assert(kernel_heap.first == kernel_heap.last && kernel_heap.first->free);
    assert(!kernel_heap.first->next &&
           kernel_heap.first->size == HEAP_INITIAL_PAGES * PAGE_SIZE -
                                      sizeof(heap_block_t));

    void *x = kmalloc(128U), *y = kmalloc(256U), *z = kmalloc(128U);
    assert(x && y && z);
    kfree(y);
    expect_free_panic(y);
    death_pointer = x;
    expect_child_panic(free_interior);
    expect_child_panic(free_misaligned);
    expect_child_panic(free_outside);
    expect_child_panic(free_after_heap);
    kfree(x); kfree(z);
    independent_check();
    fixture_destroy();

    fixture_init(8U * 1024U * 1024U, 0);
    expect_child_panic(duplicate_singleton_insert);
    expect_child_panic(reciprocal_singleton_cycle_search);
    expect_child_panic(broken_reciprocal_search);
    expect_child_panic(broken_multi_block_cycle_search);
    expect_child_panic(broken_bitmap_search);
    expect_child_panic(broken_membership_search);
    expect_child_panic(invalid_bin_head_search);
    expect_child_panic(allocated_index_corruption);
    expect_child_panic(physical_chain_corruption);
    expect_child_panic(accounting_corruption);
    expect_child_panic(header_size_corruption);
    expect_child_panic(panic_double_free);
    fixture_destroy();

    fixture_init(8U * 1024U * 1024U, 0);
    u32 maps_before = atomic_load(&vmm_map_calls);
    assert(!kmalloc(0) && !kmalloc((usize)-1));
    assert(atomic_load(&pmm_alloc_calls) == 0U);
    assert(!kmalloc(fake_heap_capacity));
    assert(atomic_load(&pmm_alloc_calls) == 0U);
    fixture_interrupts_enabled = false;
    void *interrupt_safe = kmalloc(32U);
    assert(interrupt_safe);
    kfree(interrupt_safe);
    assert(!fixture_interrupts_enabled);
    assert(!kmalloc(30000U));
    assert(!fixture_interrupts_enabled && atomic_load(&pmm_alloc_calls) == 0U);
    fixture_interrupts_enabled = true;
    atomic_store(&pmm_fail_at, 2);
    assert(!kmalloc(30000U));
    assert(atomic_load(&pmm_alloc_calls) == 3U &&
           atomic_load(&pmm_free_calls) == 2U);
    assert(atomic_load(&vmm_map_calls) == maps_before);
    assert(kernel_heap.end == (u8 *)kernel_heap.start +
                              HEAP_INITIAL_PAGES * PAGE_SIZE);

    atomic_store(&pmm_fail_at, -1);
    atomic_store(&vmm_fail_at, 2);
    assert(!kmalloc(30000U));
    assert(atomic_load(&vmm_map_calls) == 3U);
    assert(atomic_load(&pmm_alloc_calls) == 7U &&
           atomic_load(&pmm_free_calls) == 4U);
    assert(kernel_heap.end == (u8 *)kernel_heap.start +
                              (HEAP_INITIAL_PAGES + 2U) * PAGE_SIZE);
    assert(heap_get_total_size() == heap_get_free_size());
    independent_check();

    atomic_store(&vmm_fail_at, -1);
    void *large = kmalloc(30000U);
    assert(large && ((uintptr_t)large & 15U) == 0);
    independent_check();
    kfree(large);
    independent_check();
    fixture_destroy();
}

static u32 next_random(u32 *state) {
    u32 value = *state;
    value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    *state = value;
    return value;
}

static void randomized_model_test(void) {
    enum { SLOT_COUNT = 96, OPERATIONS = 12000 };
    void *pointer[SLOT_COUNT] = {0};
    usize size[SLOT_COUNT] = {0};
    u8 value[SLOT_COUNT] = {0};
    u32 random = 0x42e91b7dU;

    fixture_init(8U * 1024U * 1024U, 0);
    for (u32 operation = 0; operation < OPERATIONS; operation++) {
        u32 slot = next_random(&random) % SLOT_COUNT;
        if (pointer[slot]) {
            const u8 *bytes = pointer[slot];
            for (usize at = 0; at < size[slot]; at++)
                assert(bytes[at] == value[slot]);
            kfree(pointer[slot]);
            pointer[slot] = NULL;
            size[slot] = 0;
        } else {
            size[slot] = (next_random(&random) % 4096U) + 1U;
            pointer[slot] = kmalloc(size[slot]);
            assert(pointer[slot]);
            assert(!((uintptr_t)pointer[slot] & 15U));
            value[slot] = (u8)(next_random(&random) | 1U);
            memset(pointer[slot], value[slot], size[slot]);
        }
        for (u32 left = 0; left < SLOT_COUNT; left++) {
            if (!pointer[left]) continue;
            uintptr_t left_start = (uintptr_t)pointer[left];
            uintptr_t left_end = left_start + ((size[left] + 15U) & ~15U);
            for (u32 right = left + 1U; right < SLOT_COUNT; right++) {
                if (!pointer[right]) continue;
                uintptr_t right_start = (uintptr_t)pointer[right];
                uintptr_t right_end = right_start +
                    ((size[right] + 15U) & ~15U);
                assert(left_end <= right_start || right_end <= left_start);
            }
        }
        if ((operation % 127U) == 0U) independent_check();
    }
    for (u32 slot = 0; slot < SLOT_COUNT; slot++) {
        if (pointer[slot]) {
            for (usize at = 0; at < size[slot]; at++)
                assert(((u8 *)pointer[slot])[at] == value[slot]);
            kfree(pointer[slot]);
        }
    }
    independent_check();
    assert(kernel_heap.first == kernel_heap.last && kernel_heap.first->free);
    fixture_destroy();
}

static void split_threshold_test(void) {
    const usize initial_payload = HEAP_INITIAL_PAGES * PAGE_SIZE -
                                  sizeof(heap_block_t);
    fixture_init(HEAP_INITIAL_PAGES * PAGE_SIZE,
                 HEAP_INITIAL_PAGES * PAGE_SIZE);
    void *exact = kmalloc(initial_payload);
    assert(exact && kernel_heap.first == kernel_heap.last &&
           kernel_heap.first->size == initial_payload &&
           heap_get_used_size() == initial_payload &&
           heap_get_free_size() == 0U);
    independent_check();
    kfree(exact);
    independent_check();
    fixture_destroy();

    fixture_init(HEAP_INITIAL_PAGES * PAGE_SIZE,
                 HEAP_INITIAL_PAGES * PAGE_SIZE);
    void *unsplit = kmalloc(initial_payload - 64U);
    assert(unsplit && kernel_heap.first == kernel_heap.last &&
           kernel_heap.first->size == initial_payload &&
           heap_get_used_size() == initial_payload);
    independent_check();
    kfree(unsplit);
    independent_check();
    fixture_destroy();

    fixture_init(HEAP_INITIAL_PAGES * PAGE_SIZE,
                 HEAP_INITIAL_PAGES * PAGE_SIZE);
    void *minimum_split = kmalloc(initial_payload - 80U);
    assert(minimum_split && kernel_heap.first->next &&
           kernel_heap.first->next == kernel_heap.last &&
           kernel_heap.first->next->size == 32U &&
           heap_get_total_size() == initial_payload - sizeof(heap_block_t) &&
           heap_get_used_size() + heap_get_free_size() ==
               heap_get_total_size());
    independent_check();
    kfree(minimum_split);
    independent_check();
    fixture_destroy();
}

static void bin_boundary_test(void) {
    fixture_init(2U * 1024U * 1024U, 0);
    void *a = kmalloc(16U), *guard_a = kmalloc(16U);
    void *b = kmalloc(32U), *guard_b = kmalloc(16U);
    void *c = kmalloc(64U), *guard_c = kmalloc(16U);
    assert(a && guard_a && b && guard_b && c && guard_c);
    kfree(a); kfree(b); kfree(c);
    assert(heap_free_bins[0] && heap_free_bins[0]->size == 16U);
    assert(heap_free_bins[1] && heap_free_bins[1]->size == 32U);
    assert(heap_free_bins[2] && heap_free_bins[2]->size == 64U);
    independent_check();
    kfree(guard_a); kfree(guard_b); kfree(guard_c);
    independent_check();
    fixture_destroy();
}

static void coalescing_transition_test(void) {
    fixture_init(2U * 1024U * 1024U, 0);
    void *a = kmalloc(64U), *b = kmalloc(64U), *c = kmalloc(64U);
    void *guard = kmalloc(64U);
    assert(a && b && c && guard);

    kfree(a); /* isolated free block */
    independent_check();
    kfree(c); /* free block followed by allocated guard */
    independent_check();
    kfree(b); /* merges with both physical neighbors */
    independent_check();
    assert(kernel_heap.first->free && kernel_heap.first->next &&
           !kernel_heap.first->next->free);
    kfree(guard); /* merges with the preceding run and free tail */
    independent_check();
    assert(kernel_heap.first == kernel_heap.last &&
           kernel_heap.first->free && !kernel_heap.first->next &&
           !kernel_heap.first->prev);
    fixture_destroy();
}

typedef struct {
    u32 id;
    u32 random;
    void *pointers[12];
    usize sizes[12];
    u8 values[12];
} worker_state_t;

static void *heap_worker(void *argument) {
    worker_state_t *worker = argument;
    for (u32 operation = 0; operation < 1800U; operation++) {
        u32 slot = next_random(&worker->random) % 12U;
        if (worker->pointers[slot]) {
            u8 *bytes = worker->pointers[slot];
            for (usize at = 0; at < worker->sizes[slot]; at++)
                assert(bytes[at] == worker->values[slot]);
            kfree(worker->pointers[slot]);
            worker->pointers[slot] = NULL;
        } else {
            worker->sizes[slot] = next_random(&worker->random) % 4096U + 1U;
            worker->pointers[slot] = kmalloc(worker->sizes[slot]);
            assert(worker->pointers[slot]);
            worker->values[slot] = (u8)(worker->id * 29U + slot * 7U + 1U);
            memset(worker->pointers[slot], worker->values[slot],
                   worker->sizes[slot]);
        }
    }
    for (u32 slot = 0; slot < 12U; slot++) {
        if (!worker->pointers[slot]) continue;
        for (usize at = 0; at < worker->sizes[slot]; at++)
            assert(((u8 *)worker->pointers[slot])[at] == worker->values[slot]);
        kfree(worker->pointers[slot]);
    }
    return NULL;
}

static void concurrent_test(void) {
    enum { WORKERS = 6 };
    pthread_t threads[WORKERS];
    worker_state_t workers[WORKERS] = {0};
    fixture_init(8U * 1024U * 1024U, 0);
    for (u32 i = 0; i < WORKERS; i++) {
        workers[i].id = i + 1U;
        workers[i].random = 0x91e10da5U ^ (i * 0x9e3779b9U);
        assert(pthread_create(&threads[i], NULL, heap_worker, &workers[i]) == 0);
    }
    for (u32 i = 0; i < WORKERS; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    independent_check();
    assert(kernel_heap.first == kernel_heap.last && kernel_heap.first->free);
    fixture_destroy();
}

static int compare_u64(const void *left, const void *right) {
    u64 a = *(const u64 *)left, b = *(const u64 *)right;
    return (a > b) - (a < b);
}

static void benchmark(void) {
    enum { BLOCKS = 4096, REPETITIONS = 2000 };
    void **pointers = calloc(BLOCKS, sizeof(*pointers));
    u64 *latencies = calloc(REPETITIONS, sizeof(*latencies));
    assert(pointers && latencies);
    fixture_init(64U * 1024U * 1024U, 64U * 1024U * 1024U);
    for (u32 i = 0; i < BLOCKS; i++) {
        pointers[i] = kmalloc(16U);
        assert(pointers[i]);
    }
    for (u32 i = 0; i < BLOCKS; i += 2U) kfree(pointers[i]);
    assert(heap_free_find_locked(512U) == kernel_heap.last);
    independent_check();
    assert(heap_get_used_size() == (u64)(BLOCKS / 2U) * 16U);
    atomic_store(&heap_lock_total_ns, 0U);
    atomic_store(&heap_lock_max_ns, 0U);
    for (u32 i = 0; i < REPETITIONS; i++) {
        u64 started = monotonic_ns();
        void *probe = kmalloc(512U);
        latencies[i] = monotonic_ns() - started;
        assert(probe);
        kfree(probe);
    }
    independent_check();
    if (heap_get_used_size() != (u64)(BLOCKS / 2U) * 16U) {
        for (heap_block_t *block = kernel_heap.first; block; block = block->next)
            if (!block->free)
                fprintf(stderr, "live block at +%llu bytes size=%zu\n",
                        (unsigned long long)((u8 *)block - (u8 *)kernel_heap.start),
                        block->size);
    }
    assert(heap_get_used_size() == (u64)(BLOCKS / 2U) * 16U);
    qsort(latencies, REPETITIONS, sizeof(*latencies), compare_u64);
    u64 total_lock_ns = atomic_load(&heap_lock_total_ns);
    printf("heap benchmark: fragmented 4096-block search; 2000 512-byte alloc/free pairs\n");
    printf("allocation latency ns: p50=%llu p95=%llu p99=%llu max=%llu\n",
           (unsigned long long)latencies[REPETITIONS / 2U],
           (unsigned long long)latencies[REPETITIONS * 95U / 100U],
           (unsigned long long)latencies[REPETITIONS * 99U / 100U],
           (unsigned long long)latencies[REPETITIONS - 1U]);
    printf("heap-lock hold ns: average=%llu max=%llu; payload=%llu used=%llu free=%llu\n",
           (unsigned long long)(total_lock_ns / (REPETITIONS * 2U)),
           (unsigned long long)atomic_load(&heap_lock_max_ns),
           (unsigned long long)heap_get_total_size(),
           (unsigned long long)heap_get_used_size(),
           (unsigned long long)heap_get_free_size());
    fixture_destroy();
    free(pointers);
    free(latencies);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "benchmark") == 0) {
        benchmark();
        return 0;
    }
    basic_and_failure_test();
    split_threshold_test();
    bin_boundary_test();
    coalescing_transition_test();
    randomized_model_test();
    concurrent_test();
    puts("heap: bins, splitting/coalescing, accounting, growth failures, randomized model and concurrent churn passed");
    return 0;
}
"""


def build(binary, production, sanitizers, arguments):
    with tempfile.TemporaryDirectory(prefix="mangrove-heap-") as directory:
        source_path = Path(directory) / "heap.c"
        source_path.write_text(COMMON + production + HARNESS)
        command = [CC, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
                   "-pthread", str(source_path), "-o", str(binary)]
        if sanitizers:
            command[1:1] = ["-fsanitize=address,undefined"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary), *arguments], check=True)


with tempfile.TemporaryDirectory(prefix="mangrove-heap-bin-") as directory:
    build(Path(directory) / "heap-sanitized", PRODUCTION_TEST, True, [])
    build(Path(directory) / "heap-benchmark", PRODUCTION_BENCH, False,
          ["benchmark"])
