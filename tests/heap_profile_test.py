#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production heap-profiler accounting and loss controls."""
from pathlib import Path
import subprocess
import sys
import tempfile
import os

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"

SOURCE = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include "kernel/src/heap_profile.c"

u32 cpu_online_count(void) { return 4U; }
static bool fail_trace_storage;
void *heap_profile_test_storage_alloc(usize bytes)
{
    return fail_trace_storage ? NULL : calloc(1, bytes);
}
static pthread_mutex_t profile_commit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t admission_order_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t admission_order_condition = PTHREAD_COND_INITIALIZER;
static bool delayed_request_entered;
static bool release_delayed_request;

static void allocation(uintptr_t pointer, usize requested, u32 cpu)
{
    assert(heap_profile_request_enter());
    heap_profile_allocated_locked((void *)pointer, requested, cpu,
                                   HEAP_PROFILE_CONTEXT_NORMAL);
    heap_profile_request_leave_locked();
}

static void allocation_context(uintptr_t pointer, usize requested, u32 cpu,
                               u32 context)
{
    assert(heap_profile_request_enter());
    heap_profile_allocated_locked((void *)pointer, requested, cpu, context);
    heap_profile_request_leave_locked();
}

static void failure(usize requested)
{
    assert(heap_profile_request_enter());
    heap_profile_failed_locked(requested, 0U,
                               HEAP_PROFILE_CONTEXT_NORMAL);
    heap_profile_request_leave_locked();
}

static void failure_context(usize requested, u32 context)
{
    assert(heap_profile_request_enter());
    heap_profile_failed_locked(requested, 0U, context);
    heap_profile_request_leave_locked();
}

static void release(uintptr_t pointer, u32 cpu)
{
    heap_profile_freed_locked((void *)pointer, cpu,
                              HEAP_PROFILE_CONTEXT_NORMAL);
}

static void release_context(uintptr_t pointer, u32 cpu, u32 context)
{
    heap_profile_freed_locked((void *)pointer, cpu, context);
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
                                      HEAP_PROFILE_CONTEXT_NORMAL);
        heap_profile_request_leave_locked();
        pthread_mutex_unlock(&profile_commit_lock);

        pthread_mutex_lock(&profile_commit_lock);
        heap_profile_freed_locked((void *)pointer,
                                  (argument->cpu + 1U) % 4U,
                                  HEAP_PROFILE_CONTEXT_NORMAL);
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
                                  HEAP_PROFILE_CONTEXT_NORMAL);
    heap_profile_request_leave_locked();
    assert(pthread_mutex_unlock(&profile_commit_lock) == 0);
    return NULL;
}

static heap_profile_diag_snapshot_t *freeze(void)
{
    usize total_bytes = sizeof(heap_profile_diag_snapshot_t) +
        profile_header[HPPH_TRACE_EVENT_CAPACITY] *
        sizeof(heap_profile_event_t);
    heap_profile_diag_snapshot_t *snapshot =
        calloc(1, total_bytes);
    assert(snapshot);
    heap_profile_freeze_locked();
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

int main(void)
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
    assert(snapshot->header[HPPH_LIFETIME_SAMPLES] == 2U);
    assert(snapshot->header[HPPH_LIFETIME_OPERATIONS] == 4U);
    assert(snapshot->lifetime[7][1] == 2U); /* each lifetime is two ops */
    assert(snapshot->header[HPPH_TRACE_EVENT_COUNT] == 4U);
    assert(snapshot->events[0].kind == HEAP_PROFILE_EVENT_ALLOCATED);
    assert(snapshot->events[0].object_id == 1U);
    assert(snapshot->events[1].object_id == 2U);
    assert(snapshot->events[2].kind == HEAP_PROFILE_EVENT_FREED);
    assert(snapshot->events[2].object_id == 1U);
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

    snapshot = freeze();
    assert(snapshot->header[HPPH_MAGIC] == HEAP_PROFILE_DIAG_MAGIC);
    assert(snapshot->header[HPPH_TOTAL_BYTES] ==
           sizeof(*snapshot) + HEAP_PROFILE_EVENT_CAPACITY *
               sizeof(heap_profile_event_t));
    assert(snapshot->header[HPPH_ATTEMPTS] == 5U);
    assert(snapshot->header[HPPH_SUCCESSES] == 4U);
    assert(snapshot->header[HPPH_FAILURES] == 1U);
    assert(snapshot->header[HPPH_FREES] == 3U);
    assert(snapshot->header[HPPH_CROSS_CPU_FREES] == 1U);
    assert(snapshot->header[HPPH_TRACKED_LIVE] == 1U);
    assert(snapshot->header[HPPH_TRACE_EVENT_COUNT] == 8U);
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
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-heap-profile-") as directory:
    source = Path(directory) / "profile_test.c"
    binary = Path(directory) / "profile_test"
    source.write_text(SOURCE)
    subprocess.run([
        CC, "-std=gnu11", "-O1", "-g", "-pthread", "-DHEAP_PROFILE_DIAGNOSTICS=1",
        "-DHEAP_PROFILE_HOST_TEST=1",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", str(ROOT / "include"), "-I", str(ROOT / "kernel/include"),
        "-I", str(ROOT / "tests"), "-I", str(ROOT),
        "-idirafter", str(ROOT / "libc/include"), str(source), "-o",
        str(binary)
    ], check=True)
    subprocess.run([str(binary)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

print("Production heap-profiler accounting, boundary, and loss tests: PASS")
