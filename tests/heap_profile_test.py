#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production heap-profiler accounting and loss controls."""
from pathlib import Path
import subprocess
import sys
import tempfile
import os
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"

SOURCE = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "kernel/src/heap_profile.c"

u32 cpu_online_count(void) { return 4U; }
static bool fail_trace_storage;
void *heap_profile_test_storage_alloc(usize bytes)
{
    return fail_trace_storage ? NULL : calloc(1, bytes);
}
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
static const u16 mock_slab_sizes[SLAB_CLASS_COUNT] = {
    16U,32U,48U,64U,80U,96U,112U,128U,144U,160U,176U,192U,208U,
    224U,240U,256U,288U,320U,352U,384U
};
bool slab_is_initialized(void) { return true; }
u32 slab_cpu_cache_count_current(void) { return 4U; }
u64 slab_cpu_cache_metadata_bytes(void) { return 1024U; }
u16 slab_class_size(u32 index)
{
    return index < SLAB_CLASS_COUNT ? mock_slab_sizes[index] : 0U;
}
bool slab_get_class_stats(u32 index, slab_class_stats_t *stats)
{
    if (index >= SLAB_CLASS_COUNT || !stats)
        return false;
    *stats = (slab_class_stats_t){0};
    return true;
}
bool slab_get_cpu_class_stats(u32 cpu, u32 index,
                              slab_cpu_class_stats_t *stats)
{
    if (cpu >= 4U || index >= SLAB_CLASS_COUNT || !stats)
        return false;
    *stats = (slab_cpu_class_stats_t){0};
    if (cpu == 1U && index == 0U) {
        stats->allocations = 3U;
        stats->hits = 2U;
        stats->misses = 1U;
        stats->refills = 1U;
        stats->cached_objects = 1U;
    }
    return true;
}
bool slab_get_classifier_stats(slab_classifier_stats_t *stats)
{
    if (!stats)
        return false;
    *stats = (slab_classifier_stats_t){0};
    return true;
}
u64 heap_profile_mapped_bytes(void) { return 0x100000U; }
u64 heap_get_total_size(void) { return 0x80000U; }
u64 heap_get_used_size(void) { return 0x20000U; }
u64 heap_get_free_size(void) { return 0x60000U; }
#endif
static pthread_mutex_t profile_commit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t admission_order_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t admission_order_condition = PTHREAD_COND_INITIALIZER;
static bool delayed_request_entered;
static bool release_delayed_request;

static void allocation(uintptr_t pointer, usize requested, u32 cpu)
{
    assert(heap_profile_request_enter());
    heap_profile_allocated_locked((void *)pointer, requested, cpu,
                                   HEAP_PROFILE_CONTEXT_NORMAL, 0x101U);
    heap_profile_request_leave_locked();
}

static void allocation_context(uintptr_t pointer, usize requested, u32 cpu,
                               u32 context)
{
    assert(heap_profile_request_enter());
    heap_profile_allocated_locked((void *)pointer, requested, cpu, context,
                                   0x202U);
    heap_profile_request_leave_locked();
}

static void failure(usize requested)
{
    assert(heap_profile_request_enter());
    heap_profile_failed_locked(requested, 0U,
                               HEAP_PROFILE_CONTEXT_NORMAL, 0x303U);
    heap_profile_request_leave_locked();
}

static void failure_context(usize requested, u32 context)
{
    assert(heap_profile_request_enter());
    heap_profile_failed_locked(requested, 0U, context, 0x404U);
    heap_profile_request_leave_locked();
}

static void release(uintptr_t pointer, u32 cpu)
{
    heap_profile_freed_locked((void *)pointer, cpu,
                              HEAP_PROFILE_CONTEXT_NORMAL, 0x505U);
}

static void release_context(uintptr_t pointer, u32 cpu, u32 context)
{
    heap_profile_freed_locked((void *)pointer, cpu, context, 0x606U);
}

typedef struct {
    u32 cpu;
    u32 rounds;
} worker_argument_t;

static void *worker(void *opaque)
{
    worker_argument_t *argument = opaque;
    uintptr_t pointer = 0xffffc10000000000ULL +
                        (uintptr_t)argument->cpu * 0x10000U;
    usize requested = 16U + argument->cpu * 16U;
    for (u32 i = 0; i < argument->rounds; i++) {
        assert(heap_profile_request_enter());
        pthread_mutex_lock(&profile_commit_lock);
        heap_profile_allocated_locked((void *)pointer, requested,
                                      argument->cpu,
                                      HEAP_PROFILE_CONTEXT_NORMAL,
                                      0x707U);
        heap_profile_request_leave_locked();
        pthread_mutex_unlock(&profile_commit_lock);

        pthread_mutex_lock(&profile_commit_lock);
        heap_profile_freed_locked((void *)pointer,
                                  (argument->cpu + 1U) % 4U,
                                  HEAP_PROFILE_CONTEXT_NORMAL, 0x707U);
        pthread_mutex_unlock(&profile_commit_lock);
    }
    return NULL;
}

static void *delayed_profile_commit(void *opaque)
{
    uintptr_t pointer = (uintptr_t)opaque;
    assert(heap_profile_request_enter());
    assert(pthread_mutex_lock(&admission_order_lock) == 0);
    delayed_request_entered = true;
    assert(pthread_cond_broadcast(&admission_order_condition) == 0);
    while (!release_delayed_request)
        assert(pthread_cond_wait(&admission_order_condition,
                                 &admission_order_lock) == 0);
    assert(pthread_mutex_unlock(&admission_order_lock) == 0);

    assert(pthread_mutex_lock(&profile_commit_lock) == 0);
    heap_profile_allocated_locked((void *)pointer, 128U, 0U,
                                  HEAP_PROFILE_CONTEXT_NORMAL, 0x808U);
    heap_profile_request_leave_locked();
    assert(pthread_mutex_unlock(&profile_commit_lock) == 0);
    return NULL;
}

static heap_profile_diag_snapshot_t *freeze(void)
{
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    usize total_bytes = profile_header[HPPH_TOTAL_BYTES];
#else
    usize total_bytes = sizeof(heap_profile_diag_snapshot_t) +
        profile_header[HPPH_TRACE_EVENT_CAPACITY] *
        sizeof(heap_profile_event_t);
#endif
    heap_profile_diag_snapshot_t *snapshot =
        calloc(1, total_bytes);
    assert(snapshot);
    heap_profile_freeze_locked();
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    heap_profile_hybrid_capture();
#endif
    for (usize offset = 0; offset < total_bytes; ) {
        usize amount = total_bytes - offset;
        if (amount > HEAP_PROFILE_EXPORT_CHUNK_BYTES)
            amount = HEAP_PROFILE_EXPORT_CHUNK_BYTES;
        assert(heap_profile_copy_frozen_range((u8 *)snapshot + offset,
                                              offset, amount));
        offset += amount;
    }
    return snapshot;
}

static void save_snapshot(const char *path,
                          const heap_profile_diag_snapshot_t *snapshot)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    usize length = snapshot->header[HPPH_TOTAL_BYTES];
    assert(fwrite(snapshot, 1U, length, file) == length);
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    static const u32 expected_classes[HEAP_PROFILE_MAX_CLASSES] = {
        16U,32U,48U,64U,80U,96U,112U,128U,
        144U,160U,176U,192U,208U,224U,240U,256U,
        288U,320U,352U,384U,416U,448U,480U,512U,
        576U,640U,704U,768U,832U,896U,960U,1024U
    };
    static const u32 expected_counts[HEAP_PROFILE_CANDIDATES] = {
        16U,20U,24U,28U,32U
    };
    static const u32 expected_cutoffs[HEAP_PROFILE_CANDIDATES] = {
        256U,384U,512U,768U,1024U
    };
    const uintptr_t first = 0xffffc00000000100ULL;
    const uintptr_t reused = 0xffffc00000010100ULL;
    const uintptr_t anchor = 0xffffc00000020100ULL;
    const uintptr_t unfreed = 0xffffc00000030100ULL;
    heap_profile_diag_snapshot_t *snapshot;

    heap_profile_init();
    for (u32 candidate = 0; candidate < HEAP_PROFILE_CANDIDATES;
         candidate++) {
        assert(profile_candidate_summary[candidate][CANDIDATE_CUTOFF] ==
               expected_cutoffs[candidate]);
        assert(profile_candidate_summary[candidate][CANDIDATE_CLASS_COUNT] ==
               expected_counts[candidate]);
        for (u32 index = 0; index < expected_counts[candidate]; index++) {
            u32 actual_index = (u32)-1;
            assert(profile_candidate_class_size(index) ==
                   expected_classes[index]);
            assert(profile_candidate_index(candidate,
                                           expected_classes[index],
                                           &actual_index));
            assert(actual_index == index);
        }
        u32 ignored = 0;
        assert(!profile_candidate_index(candidate,
                    (u64)expected_cutoffs[candidate] + 1U, &ignored));
    }

    /* Admission/start order is deliberately opposite to heap-lock commit
     * order. The profile age is assigned only when the operation commits. */
    heap_profile_init();
    pthread_t delayed;
    delayed_request_entered = false;
    release_delayed_request = false;
    assert(pthread_create(&delayed, NULL, delayed_profile_commit,
                          (void *)(uintptr_t)anchor) == 0);
    assert(pthread_mutex_lock(&admission_order_lock) == 0);
    while (!delayed_request_entered)
        assert(pthread_cond_wait(&admission_order_condition,
                                 &admission_order_lock) == 0);
    assert(pthread_mutex_unlock(&admission_order_lock) == 0);
    allocation(reused, 128U, 1U); /* enters later, commits first */
    assert(pthread_mutex_lock(&admission_order_lock) == 0);
    release_delayed_request = true;
    assert(pthread_cond_broadcast(&admission_order_condition) == 0);
    assert(pthread_mutex_unlock(&admission_order_lock) == 0);
    assert(pthread_join(delayed, NULL) == 0);
    assert(profile_header[HPPH_OPERATIONS] == 2U);

    release(reused, 1U);
    release(anchor, 0U);
    snapshot = freeze();
    if (argc > 1)
        save_snapshot(argv[1], snapshot);
    assert(snapshot->header[HPPH_LIFETIME_SAMPLES] == 2U);
    assert(snapshot->header[HPPH_LIFETIME_OPERATIONS] == 4U);
    assert(snapshot->lifetime[7][1] == 2U); /* each lifetime is two ops */
    assert(snapshot->header[HPPH_TRACE_EVENT_COUNT] == 4U);
    assert(snapshot->events[0].kind == HEAP_PROFILE_EVENT_ALLOCATED);
    assert(snapshot->events[0].object_id == 1U);
    assert(snapshot->events[1].object_id == 2U);
    assert(snapshot->events[2].kind == HEAP_PROFILE_EVENT_FREED);
    assert(snapshot->events[2].object_id == 1U);
    assert(snapshot->events[0].caller_text_offset == 0x101U);
    assert(snapshot->events[1].caller_text_offset == 0x808U);
    assert(snapshot->events[2].caller_text_offset == 0x505U);
    assert(snapshot->header[HPPH_FLAGS] == 0U);
    free(snapshot);

    /* Address reuse is a new lifetime; failed requests do not create objects. */
    heap_profile_init();
    allocation(reused, 16U, 1U);
    allocation(anchor, 32U, 0U);
    assert(heap_profile_set_phase_locked(1U));

    release(reused, 0U);             /* remote free */
    allocation(reused, 16U, 0U);     /* address reuse is a new lifetime */
    release(reused, 0U);
    release(anchor, 0U);
    failure(64U);
    allocation(unfreed, 64U, 1U);   /* remains live at the snapshot */
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    bool route_admitted = heap_profile_hybrid_enter();
    assert(route_admitted);
    heap_profile_hybrid_alloc_complete(
        route_admitted, HEAP_PROFILE_HYBRID_SLAB_SUCCESS,
        HEAP_PROFILE_CONTEXT_NORMAL);
    route_admitted = heap_profile_hybrid_enter();
    assert(route_admitted);
    heap_profile_hybrid_alloc_complete(
        route_admitted, HEAP_PROFILE_HYBRID_HEAP_DIRECT_SUCCESS,
        HEAP_PROFILE_CONTEXT_NORMAL);
    route_admitted = heap_profile_hybrid_enter();
    assert(route_admitted);
    heap_profile_hybrid_free_complete(route_admitted, true,
                                      HEAP_PROFILE_CONTEXT_NORMAL);
    route_admitted = heap_profile_hybrid_enter();
    assert(route_admitted);
    heap_profile_hybrid_free_complete(route_admitted, false,
                                      HEAP_PROFILE_CONTEXT_NORMAL);
    route_admitted = heap_profile_hybrid_enter();
    assert(route_admitted);
    heap_profile_hybrid_reclaim_complete(route_admitted, true, 2U, 0U, 1U);
    heap_profile_hybrid_reclaim_retry(route_admitted);
    heap_profile_hybrid_alloc_complete(
        route_admitted, HEAP_PROFILE_HYBRID_ALLOCATION_FAILURE,
        HEAP_PROFILE_CONTEXT_NORMAL);
    route_admitted = heap_profile_hybrid_enter();
    assert(route_admitted);
    heap_profile_hybrid_backing_allocation_complete(route_admitted, 2U);
#endif

    snapshot = freeze();
    if (argc > 2)
        save_snapshot(argv[2], snapshot);
    assert(snapshot->header[HPPH_MAGIC] == HEAP_PROFILE_DIAG_MAGIC);
    assert(snapshot->header[HPPH_TOTAL_BYTES] ==
           sizeof(*snapshot) + HEAP_PROFILE_EVENT_CAPACITY *
               sizeof(heap_profile_event_t)
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
           + sizeof(heap_profile_hybrid_snapshot_t)
#endif
           );
    assert(snapshot->header[HPPH_ATTEMPTS] == 5U);
    assert(snapshot->header[HPPH_SUCCESSES] == 4U);
    assert(snapshot->header[HPPH_FAILURES] == 1U);
    assert(snapshot->header[HPPH_FREES] == 3U);
    assert(snapshot->header[HPPH_CROSS_CPU_FREES] == 1U);
    assert(snapshot->header[HPPH_TRACKED_LIVE] == 1U);
    assert(snapshot->header[HPPH_TRACE_EVENT_COUNT] == 8U);
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    heap_profile_hybrid_snapshot_t *runtime_snapshot = (void *)
        ((u8 *)snapshot + sizeof(*snapshot) +
         HEAP_PROFILE_EVENT_CAPACITY * sizeof(heap_profile_event_t));
    assert(runtime_snapshot->routes[HPPHY_ROUTE_KMALLOC_ATTEMPTS] == 3U);
    assert(runtime_snapshot->routes[HPPHY_ROUTE_SLAB_SUCCESS] == 1U);
    assert(runtime_snapshot->routes[HPPHY_ROUTE_HEAP_DIRECT_SUCCESS] == 1U);
    assert(runtime_snapshot->routes[HPPHY_ROUTE_ALLOCATION_FAILURE] == 1U);
    assert(runtime_snapshot->routes[HPPHY_ROUTE_SLAB_FREES] == 1U);
    assert(runtime_snapshot->routes[HPPHY_ROUTE_HEAP_FREES] == 1U);
    assert(runtime_snapshot->routes[HPPHY_ROUTE_RECLAIM_ATTEMPTS] == 1U);
    assert(runtime_snapshot->routes[HPPHY_ROUTE_RECLAIM_CACHED_OBJECTS] == 2U);
    assert(runtime_snapshot->routes[HPPHY_ROUTE_RECLAIM_EMPTY_BACKING_PAGES] == 1U);
    assert(runtime_snapshot->routes[HPPHY_ROUTE_RECLAIM_ALLOC_RETRIES] == 1U);
    assert(runtime_snapshot->routes[
        HPPHY_ROUTE_SLAB_BACKING_ALLOCATION_FAILURES] == 2U);
#endif
    assert(snapshot->events[0].object_id == 1U);
    assert(snapshot->events[2].object_id == 1U);
    assert(snapshot->events[3].object_id == 4U);
    assert(snapshot->events[4].object_id == 4U);
    assert(snapshot->events[6].kind == HEAP_PROFILE_EVENT_FAILED);
    assert(snapshot->events[7].object_id == 8U);
    assert(snapshot->sizes[16][1] == 2U);
    assert(snapshot->sizes[16][3] == 2U);
    assert(snapshot->sizes[32][10] == 1U);
    assert(snapshot->sizes[32][11] == 4U);
    assert(snapshot->sizes[64][2] == 1U);
    assert(snapshot->sizes[64][6] == 1U);
    assert(snapshot->ranges[0][2] == 1U);
    assert(snapshot->ranges[1][0] == 1U);
    assert(snapshot->ranges[1][3] == 64U);
    assert(snapshot->phases[0][16][6] == 1U);
    assert(snapshot->phases[0][32][6] == 1U);
    assert(snapshot->phases[0][16][7] == 16U);
    assert(snapshot->candidate_summary[2][CANDIDATE_CUTOFF] == 512U);
    assert(snapshot->candidate[2][0][CANDIDATE_CLASS_SIZE] == 16U);
    assert((4096U - SLAB_OBJECT_AREA_OFFSET) /
           (16U + SLAB_SLOT_METADATA_BYTES) == 221U);
    assert(snapshot->candidate[2][0][CANDIDATE_ALLOCATIONS] == 2U);
    assert(snapshot->candidate[2][0][CANDIDATE_PEAK_LIVE] == 1U);
    assert(snapshot->candidate[2][3][CANDIDATE_CLASS_SIZE] == 64U);
    assert(snapshot->candidate[2][3][CANDIDATE_CURRENT_LIVE] == 1U);
    assert(snapshot->candidate_phase[1][2][3][PHASE_LIVE_AT_END] == 1U);
    assert(snapshot->candidate_summary[2][5] == 1U);
    assert(snapshot->candidate_summary[2][9] == 1U);
    assert(snapshot->candidate_summary[2][10] == 2U);
    assert(snapshot->candidate_summary[2][11] == 60U);
    assert(snapshot->candidate_summary[2][12] == 338U);
    assert(snapshot->candidate_summary[2][13] == 336U);
    assert(snapshot->candidate_phase_summary[1][2]
        [CANDIDATE_PHASE_PEAK_SLABS] == 2U);
    assert(snapshot->candidate_phase_summary[1][2]
        [CANDIDATE_PHASE_PEAK_CAPACITY] == 338U);
    assert(snapshot->candidate_cpu_mask[2][0][0] == 3U);
    assert(snapshot->lifetime[4][1] == 1U); /* one to two ops */
    assert(snapshot->lifetime[4][0] == 1U); /* one op */
    assert(snapshot->lifetime[5][2] == 1U); /* four ops */
    assert(snapshot->cpu[0][2] == 1U);
    assert(snapshot->header[HPPH_FLAGS] == 0U);
    free(snapshot);

    /* Execution-context counters are independently checked against request
     * outcomes. This validates classification accounting, not kernel context
     * behavior; native profiles provide the observed call-site contexts. */
    heap_profile_init();
    allocation_context(first, 16U, 0U, HEAP_PROFILE_CONTEXT_NORMAL);
    allocation_context(reused, 32U, 1U,
                       HEAP_PROFILE_CONTEXT_IRQ_DISABLED);
    allocation_context(anchor, 48U, 2U,
                       HEAP_PROFILE_CONTEXT_SPINLOCK_HELD);
    failure_context(64U, HEAP_PROFILE_CONTEXT_NORMAL);
    failure_context(80U, HEAP_PROFILE_CONTEXT_IRQ_DISABLED);
    failure_context(96U, HEAP_PROFILE_CONTEXT_SPINLOCK_HELD);
    release_context(first, 0U, HEAP_PROFILE_CONTEXT_NORMAL);
    release_context(reused, 1U, HEAP_PROFILE_CONTEXT_IRQ_DISABLED);
    release_context(anchor, 2U, HEAP_PROFILE_CONTEXT_SPINLOCK_HELD);
    snapshot = freeze();
    assert(snapshot->header[HPPH_ALLOC_CONTEXT_NORMAL] == 1U);
    assert(snapshot->header[HPPH_ALLOC_CONTEXT_IRQ_DISABLED] == 1U);
    assert(snapshot->header[HPPH_ALLOC_CONTEXT_SPINLOCK_HELD] == 1U);
    assert(snapshot->header[HPPH_FAIL_CONTEXT_NORMAL] == 1U);
    assert(snapshot->header[HPPH_FAIL_CONTEXT_IRQ_DISABLED] == 1U);
    assert(snapshot->header[HPPH_FAIL_CONTEXT_SPINLOCK_HELD] == 1U);
    assert(snapshot->header[HPPH_FREE_CONTEXT_NORMAL] == 1U);
    assert(snapshot->header[HPPH_FREE_CONTEXT_IRQ_DISABLED] == 1U);
    assert(snapshot->header[HPPH_FREE_CONTEXT_SPINLOCK_HELD] == 1U);
    assert(snapshot->header[HPPH_FLAGS] == 0U);
    free(snapshot);

    /* A snapshot taken while a kmalloc request is between entry and its
     * commit cannot claim complete event/accounting coverage. */
    heap_profile_init();
    assert(heap_profile_request_enter());
    snapshot = freeze();
    assert(snapshot->header[HPPH_INFLIGHT_AT_FREEZE] == 1U);
    assert(snapshot->header[HPPH_FLAGS] & HPPF_INFLIGHT_AT_FREEZE);
    assert(!heap_profile_request_enter());
    assert(!heap_profile_copy_frozen_range(
        snapshot, profile_header[HPPH_TOTAL_BYTES], 1U));
    heap_profile_request_leave_locked();
    free(snapshot);

    /* Probe traversal is bounded and marks a collision-heavy loss instead of
     * looping or claiming full lifetime reconstruction. */
    heap_profile_init();
    u64 target = 0xffffc00000050100ULL;
    u32 base = profile_hash(target);
    for (u32 i = 0; i < PROFILE_PROBE_LIMIT; i++) {
        heap_profile_live_t *entry = &profile_live[(base + i) & PROFILE_TRACK_MASK];
        entry->pointer = target + (u64)(i + 1U) * 0x1000U;
        entry->requested = 16U;
        entry->operation = 1U;
        entry->metadata = profile_metadata(PROFILE_STATE_LIVE, 0U, 0U,
                                           0U);
    }
    allocation(target, 16U, 0U);
    snapshot = freeze();
    assert(snapshot->header[HPPH_FLAGS] & HPPF_TRACKING_INCOMPLETE);
    assert(snapshot->header[HPPH_TRACKING_MISSES] == 1U);
    assert(snapshot->header[HPPH_MAX_PROBE] == PROFILE_PROBE_LIMIT);
    free(snapshot);

    /* Saturating the in-flight counter cannot wrap into a falsely complete
     * snapshot; the overflow is preserved as an incomplete-evidence flag. */
    heap_profile_init();
    __atomic_store_n(&profile_inflight, UINT32_MAX, __ATOMIC_SEQ_CST);
    assert(!heap_profile_request_enter());
    snapshot = freeze();
    assert(snapshot->header[HPPH_INFLIGHT_AT_FREEZE] == UINT32_MAX);
    assert(snapshot->header[HPPH_FLAGS] & HPPF_INFLIGHT_AT_FREEZE);
    free(snapshot);

    /* Saturation is an explicit invalidation signal, never integer wrap. */
    heap_profile_init();
    profile_header[HPPH_FAILURES] = UINT64_MAX;
    failure(8U);
    snapshot = freeze();
    assert(snapshot->header[HPPH_FAILURES] == UINT64_MAX);
    assert(snapshot->header[HPPH_FLAGS] & HPPF_COUNTER_SATURATED);
    free(snapshot);

    /* Real host worker interleavings pass through the production profile
     * commit hooks under the same serialization model as heap metadata. */
    heap_profile_init();
    pthread_t threads[4];
    worker_argument_t arguments[4];
    for (u32 cpu = 0; cpu < 4U; cpu++) {
        arguments[cpu].cpu = cpu;
        arguments[cpu].rounds = 1000U;
        assert(pthread_create(&threads[cpu], NULL, worker,
                              &arguments[cpu]) == 0);
    }
    for (u32 cpu = 0; cpu < 4U; cpu++)
        assert(pthread_join(threads[cpu], NULL) == 0);
    snapshot = freeze();
    assert(snapshot->header[HPPH_SUCCESSES] == 4000U);
    assert(snapshot->header[HPPH_FREES] == 4000U);
    assert(snapshot->header[HPPH_OPERATIONS] == 8000U);
    assert(snapshot->header[HPPH_CROSS_CPU_FREES] == 4000U);
    assert(snapshot->header[HPPH_TRACKED_LIVE] == 0U);
    assert(snapshot->header[HPPH_LIFETIME_SAMPLES] == 4000U);
    for (u32 cpu = 0; cpu < 4U; cpu++) {
        assert(snapshot->cpu[cpu][0] == 1000U);
        assert(snapshot->cpu[cpu][1] == 1000U);
        assert(snapshot->cpu[cpu][2] == 1000U);
    }
    assert(snapshot->header[HPPH_FLAGS] == 0U);
    free(snapshot);

    /* The trace never overwrites a reserved event slot or reports a complete
     * trace after a bounded-capacity loss. */
    heap_profile_init();
    profile_header[HPPH_TRACE_EVENT_COUNT] = HEAP_PROFILE_EVENT_CAPACITY;
    failure(8U);
    snapshot = freeze();
    assert(snapshot->header[HPPH_TRACE_EVENT_COUNT] ==
           HEAP_PROFILE_EVENT_CAPACITY);
    assert(snapshot->header[HPPH_FLAGS] & HPPF_TRACE_OVERFLOW);
    free(snapshot);

    /* Trace backing failure leaves aggregate profiling usable while making
     * the ordered-event product explicitly incomplete. */
    free(profile_events);
    profile_events = NULL;
    fail_trace_storage = true;
    heap_profile_init();
    allocation(anchor, 16U, 0U);
    snapshot = freeze();
    assert(snapshot->header[HPPH_TRACE_EVENT_CAPACITY] == 0U);
    assert(snapshot->header[HPPH_TRACE_EVENT_COUNT] == 0U);
    assert(snapshot->header[HPPH_FLAGS] & HPPF_TRACE_UNAVAILABLE);
    assert(snapshot->header[HPPH_SUCCESSES] == 1U);
    free(snapshot);
    fail_trace_storage = false;
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
    /* Public route counters are independently closed and exported with the
     * existing slab/classifier counters. They must not depend on heap trace
     * events being emitted by a successful slab allocation. */
    heap_profile_init();
    bool admitted = heap_profile_hybrid_enter();
    assert(admitted);
    heap_profile_hybrid_alloc_complete(
        admitted, HEAP_PROFILE_HYBRID_SLAB_SUCCESS,
        HEAP_PROFILE_CONTEXT_NORMAL);
    admitted = heap_profile_hybrid_enter();
    assert(admitted);
    heap_profile_hybrid_alloc_complete(
        admitted, HEAP_PROFILE_HYBRID_HEAP_FALLBACK_SUCCESS,
        HEAP_PROFILE_CONTEXT_IRQ_DISABLED);
    admitted = heap_profile_hybrid_enter();
    assert(admitted);
    heap_profile_hybrid_free_complete(
        admitted, true, HEAP_PROFILE_CONTEXT_NORMAL);
    admitted = heap_profile_hybrid_enter();
    assert(admitted);
    heap_profile_hybrid_free_complete(
        admitted, false, HEAP_PROFILE_CONTEXT_SPINLOCK_HELD);
    bool cpu0_cache = heap_profile_hybrid_cache_path_enter(0U);
    bool cpu1_cache = heap_profile_hybrid_cache_path_enter(1U);
    assert(cpu0_cache && cpu1_cache);
    heap_profile_hybrid_cache_path_leave(1U, cpu1_cache);
    heap_profile_hybrid_cache_path_leave(0U, cpu0_cache);
    snapshot = freeze();
    if (argc > 3)
        save_snapshot(argv[3], snapshot);
    heap_profile_hybrid_snapshot_t *hybrid =
        (heap_profile_hybrid_snapshot_t *)((u8 *)snapshot +
            sizeof(*snapshot) + HEAP_PROFILE_EVENT_CAPACITY *
            sizeof(heap_profile_event_t));
    assert(hybrid->header[HPPHY_FLAGS] == 0U);
    assert(hybrid->routes[HPPHY_ROUTE_KMALLOC_ATTEMPTS] == 2U);
    assert(hybrid->routes[HPPHY_ROUTE_SLAB_SUCCESS] == 1U);
    assert(hybrid->routes[HPPHY_ROUTE_HEAP_FALLBACK_SUCCESS] == 1U);
    assert(hybrid->routes[HPPHY_ROUTE_SLAB_FREES] == 1U);
    assert(hybrid->routes[HPPHY_ROUTE_HEAP_FREES] == 1U);
    assert(hybrid->cpu_cache[1][0][HPPHY_CPU_CACHE_HITS] == 2U);
    assert(hybrid->cpu_cache[1][0][HPPHY_CPU_CACHED_OBJECTS] == 1U);
    assert(hybrid->header[HPPHY_HEAP_MAPPED_BYTES] == 0x100000U);
    assert(hybrid->header[HPPHY_MAX_CONCURRENT_CACHE_CPUS] == 2U);
    free(snapshot);

    heap_profile_init();
    admitted = heap_profile_hybrid_enter();
    assert(admitted);
    heap_profile_freeze_locked();
    heap_profile_hybrid_capture();
    assert(profile_header[HPPH_FLAGS] & HPPF_HYBRID_STATS_UNAVAILABLE);
    assert(profile_hybrid->header[HPPHY_FLAGS] & HPPHYF_ROUTE_INFLIGHT);
    heap_profile_hybrid_alloc_complete(
        admitted, HEAP_PROFILE_HYBRID_SLAB_SUCCESS,
        HEAP_PROFILE_CONTEXT_NORMAL);
#endif
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-heap-profile-") as directory:
    source = Path(directory) / "profile_test.c"
    binary = Path(directory) / "profile_test"
    hybrid_binary = Path(directory) / "hybrid_profile_test"
    snapshot = Path(directory) / "profile.bin"
    final_snapshot = Path(directory) / "profile-final.bin"
    hybrid_snapshot = Path(directory) / "hybrid-profile.bin"
    hybrid_base_snapshot = Path(directory) / "hybrid-profile-base.bin"
    hybrid_final_snapshot = Path(directory) / "hybrid-profile-final.bin"
    source.write_text(SOURCE)
    base_command = [
        CC, "-std=gnu11", "-O1", "-g", "-pthread", "-DHEAP_PROFILE_DIAGNOSTICS=1",
        "-DHEAP_PROFILE_HOST_TEST=1",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", str(ROOT / "include"), "-I", str(ROOT / "kernel/include"),
        "-I", str(ROOT / "tests"), "-I", str(ROOT),
        "-idirafter", str(ROOT / "libc/include")]
    subprocess.run([*base_command, str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(snapshot), str(final_snapshot)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    subprocess.run([*base_command, "-DHEAP_PROFILE_HYBRID_DIAGNOSTICS=1",
                    str(source), "-o", str(hybrid_binary)], check=True)
    subprocess.run([str(hybrid_binary), str(hybrid_base_snapshot),
                    str(hybrid_final_snapshot), str(hybrid_snapshot)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    spec = importlib.util.spec_from_file_location(
        "heap_profile_analyze", ROOT / "tests/heap_profile_analyze.py")
    analyzer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(analyzer)
    report = analyzer.parse_snapshot(snapshot.read_bytes(), expected_phase=0)
    allocations = {row["caller_text_offset"]: row
                   for row in report["allocation_call_sites"]}
    assert allocations[0x101]["allocations"] == 1
    assert allocations[0x101]["objects_freed"] == 1
    assert allocations[0x101]["lifetime_operation_sum"] == 2
    assert allocations[0x808]["cross_cpu_frees"] == 0
    assert len(report["free_call_sites"]) == 1
    assert report["free_call_sites"][0]["caller_text_offset"] == 0x505
    assert len(report["failure_call_sites"]) == 0

    final_report = analyzer.parse_snapshot(final_snapshot.read_bytes(),
                                           expected_phase=1)
    allocations = {row["caller_text_offset"]: row
                   for row in final_report["allocation_call_sites"]}
    assert allocations[0x101]["allocations"] == 4
    assert allocations[0x101]["objects_freed"] == 3
    assert allocations[0x101]["live_objects_at_snapshot"] == 1
    assert allocations[0x101]["lifetime_operation_sum"] == 7
    assert allocations[0x101]["cross_cpu_frees"] == 1
    assert final_report["free_call_sites"][0]["caller_text_offset"] == 0x505
    assert final_report["free_call_sites"][0]["frees"] == 3
    assert final_report["failure_call_sites"] == [{
        "caller_text_offset": 0x303,
        "failures": 1,
        "failed_requested_bytes": 64,
        "request_sizes": [{"requested_bytes": 64, "events": 1}],
    }]
    hybrid_report = analyzer.parse_snapshot(hybrid_snapshot.read_bytes())
    assert hybrid_report["hybrid_runtime"]["route_counters"][
        "kmalloc_attempts"] == 2
    assert hybrid_report["hybrid_runtime"]["per_cpu_classes"][20][
        "cache_hits"] == 2

    # Version 4 used these bytes as reserved and remains readable. The
    # analyzer must not mistake the old zero field for a meaningful RVA.
    import struct
    legacy = bytearray(snapshot.read_bytes())
    struct.pack_into("<Q", legacy, 8, 4)
    event_offset = analyzer.expected_base_bytes()
    for index in range(4):
        struct.pack_into("<I", legacy,
                         event_offset + index * analyzer.HEAP_PROFILE_EVENT_BYTES + 4,
                         0)
    legacy_report = analyzer.parse_snapshot(legacy, expected_phase=0)
    assert legacy_report["format"] == "PITH-HEAP-PROFILE-4"
    assert all(row["caller_text_offset"] == 0xffffffff
               for row in legacy_report["allocation_call_sites"])

print("Production heap-profiler accounting, boundary, and loss tests: PASS")
