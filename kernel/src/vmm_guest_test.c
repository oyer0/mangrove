/* SPDX-License-Identifier: GPL-3.0-or-later */
#if defined(SLAB_CORE_GUEST_TEST) || defined(VMM_KERNEL_RANGE_GUEST_TEST)
#include <cpu.h>
#include <cpu_relax.h>
#include <kprint.h>
#include <pmm.h>
#include <scheduler.h>
#include <spinlock.h>
#include <vmm.h>
#include <stddef.h>
#ifdef SLAB_CORE_GUEST_TEST
#include <heap.h>
#include <slab.h>
#endif

#define VMM_GUEST_TEST_SIGNATURE 0x564d4d5f53484f54ULL
#define VMM_GUEST_TEST_SPINS 100000000ULL

typedef struct {
    page_table_t *address_space;
    virt_addr_t address;
    volatile u32 ready;
    volatile u32 release;
    volatile u32 done;
    volatile u32 failed;
    volatile u32 cpu;
    spinlock_t pin_lock;
} vmm_guest_test_state_t;

static vmm_guest_test_state_t range_test_state;

static void vmm_guest_test_worker(void *argument)
{
    vmm_guest_test_state_t *state = argument;
    bool switched = false;

    for (u64 spin = 0; spin < VMM_GUEST_TEST_SPINS; spin++) {
        if (cpu_current_index() == 1U)
            break;
        if (__atomic_load_n(&state->release, __ATOMIC_ACQUIRE))
            goto done;
        (void)scheduler_yield();
    }
    if (cpu_current_index() != 1U ||
        !vmm_switch_address_space(state->address_space)) {
        __atomic_store_n(&state->failed, 1U, __ATOMIC_RELEASE);
        goto done;
    }
    switched = true;
    if (*(volatile const u64 *)(uintptr_t)state->address !=
        VMM_GUEST_TEST_SIGNATURE) {
        __atomic_store_n(&state->failed, 1U, __ATOMIC_RELEASE);
        goto restore;
    }

    /* Keep this worker on CPU 1 while leaving interrupts enabled. The
     * shootdown IPI must be serviced even though timer preemption is deferred. */
    spin_lock(&state->pin_lock);
    __atomic_store_n(&state->cpu, cpu_current_index(), __ATOMIC_RELEASE);
    __atomic_store_n(&state->ready, 1U, __ATOMIC_RELEASE);
    for (u64 spin = 0; spin < VMM_GUEST_TEST_SPINS; spin++) {
        if (__atomic_load_n(&state->release, __ATOMIC_ACQUIRE))
            break;
        cpu_relax();
    }
    if (!__atomic_load_n(&state->release, __ATOMIC_ACQUIRE))
        __atomic_store_n(&state->failed, 1U, __ATOMIC_RELEASE);
    if (switched && !vmm_switch_address_space(vmm_get_kernel_pml4()))
        __atomic_store_n(&state->failed, 1U, __ATOMIC_RELEASE);
    switched = false;
    spin_unlock(&state->pin_lock);
    goto done;

restore:
    if (switched && !vmm_switch_address_space(vmm_get_kernel_pml4()))
        __atomic_store_n(&state->failed, 1U, __ATOMIC_RELEASE);
done:
    __atomic_store_n(&state->done, 1U, __ATOMIC_RELEASE);
}

static bool vmm_guest_wait(volatile u32 *value)
{
    for (u64 spin = 0; spin < VMM_GUEST_TEST_SPINS; spin++) {
        if (__atomic_load_n(value, __ATOMIC_ACQUIRE))
            return true;
        (void)scheduler_yield();
    }
    return __atomic_load_n(value, __ATOMIC_ACQUIRE) != 0U;
}

#ifdef SLAB_CORE_GUEST_TEST
/* Retire one page through the production slab lifecycle while CPU 1 is pinned
 * in a different CR3. The VMM shootdown witnesses must therefore include the
 * remote acknowledgement before the PMM can hand the frame out again. */
static bool vmm_guest_test_slab_reclamation(u32 online)
{
    u64 acknowledgements[(CPU_MAX_COUNT + 63U) / 64U] = {0};
    u64 other_cr3[(CPU_MAX_COUNT + 63U) / 64U] = {0};
    u64 expected[(CPU_MAX_COUNT + 63U) / 64U] = {0};
    slab_class_stats_t before, allocated, retired;
    pmm_owned_frame_t reused = {0};
    phys_addr_t backing_frame = 0U;
    void *object = NULL;
    bool valid = false;

    (void)slab_cpu_drain_all();
    if (!slab_get_class_stats(0U, &before) ||
        before.live_slots || before.cached_slots || before.partial_slabs ||
        before.full_slabs || before.empty_slabs)
        return false;
#ifdef KERNEL_HYBRID_ALLOCATOR
    object = kmalloc(16U);
#else
    object = slab_alloc(16U);
#endif
    if (!object || !slab_guest_backing_frame_for_object(object,
                                                       &backing_frame) ||
        !slab_get_class_stats(0U, &allocated) ||
        allocated.backing_allocations != before.backing_allocations + 1U)
        goto cleanup;
#ifdef KERNEL_HYBRID_ALLOCATOR
    kfree(object);
#else
    if (!slab_try_free(object))
        goto cleanup;
#endif
    object = NULL;
    (void)slab_cpu_drain_all();
    if (!slab_get_class_stats(0U, &retired) || retired.live_slots ||
        retired.cached_slots || retired.backing_releases !=
                                    allocated.backing_releases + 1U ||
        !pmm_owned_frame_acquire(&reused))
        goto cleanup;

    valid = reused.physical == backing_frame;
    for (u32 index = 0; index < cpu_count(); index++) {
        cpu_local_t *cpu = cpu_by_index(index);
        if (cpu && __atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE))
            expected[index / 64U] |= 1ULL << (index % 64U);
    }
    vmm_kernel_range_guest_test_copy_shootdown_masks(
        acknowledgements, other_cr3, (CPU_MAX_COUNT + 63U) / 64U);
    for (u32 word = 0; word < (CPU_MAX_COUNT + 63U) / 64U; word++)
        if ((acknowledgements[word] & expected[word]) != expected[word])
            valid = false;
    if (online >= 2U &&
        !(other_cr3[1U / 64U] & (1ULL << (1U % 64U))))
        valid = false;
    if (!pmm_owned_frame_release(&reused))
        return false;

    kprint("VMM_RANGE_SLAB_RECLAIM pass=%u online=%u frame=%llx ack=%llx other_cr3=%llx\n",
           valid ? 1U : 0U, online,
           (unsigned long long)backing_frame,
           (unsigned long long)acknowledgements[0],
           (unsigned long long)other_cr3[0]);
    return valid;

cleanup:
    if (object) {
#ifdef KERNEL_HYBRID_ALLOCATOR
        kfree(object);
#else
        (void)slab_try_free(object);
#endif
    }
    if (reused.physical)
        (void)pmm_owned_frame_release(&reused);
    (void)slab_cpu_drain_all();
    return false;
}
#endif

bool vmm_kernel_range_guest_test_run(void)
{
    u64 acknowledgements[(CPU_MAX_COUNT + 63U) / 64U] = {0};
    u64 other_cr3[(CPU_MAX_COUNT + 63U) / 64U] = {0};
    u64 expected[(CPU_MAX_COUNT + 63U) / 64U] = {0};
    u32 online = cpu_online_count();
    u32 current = cpu_current_index();
    vmm_kernel_range_t range = {0};
    vmm_kernel_range_t stale_range = {0};
    page_table_t *kernel_root = vmm_get_kernel_pml4();
    page_table_t *other_root = 0;
    kernel_thread_t *worker = 0;
    pmm_owned_frame_t frame = {0};
    pmm_owned_frame_t premature_release = {0};
    phys_addr_t frame_physical = 0;
    phys_addr_t reused_frame = 0;
    bool mapped = false;
    bool release_blocked = false;
    bool frame_reused = false;
    bool worker_started = false;
    bool result = false;

    if (!online || online > CPU_MAX_COUNT || current >= CPU_MAX_COUNT ||
        current != 0U || !kernel_root || cpu_current()->current_pml4 != kernel_root)
        return false;
    if (online >= 2U) {
        other_root = vmm_create_address_space();
        if (!other_root)
            return false;
    }
    if (!pmm_owned_frame_acquire(&frame))
        goto cleanup;
    frame_physical = frame.physical;
    *(volatile u64 *)phys_to_virt(frame_physical) =
        VMM_GUEST_TEST_SIGNATURE;
    if (!vmm_kernel_range_reserve(1U, &range))
        goto cleanup;
    stale_range = range;
    stale_range.generation++;
    if (vmm_kernel_range_map_owned(
            &stale_range, 0U, &frame,
            PTE_READWRITE | PTE_GLOBAL | PTE_NX) ||
        vmm_kernel_range_map_owned(
            &range, 1U, &frame,
            PTE_READWRITE | PTE_GLOBAL | PTE_NX))
        goto cleanup;
    if (!vmm_kernel_range_map_owned(
            &range, 0U, &frame,
            PTE_READWRITE | PTE_GLOBAL | PTE_NX))
        goto cleanup;
    mapped = true;

    if (online >= 2U) {
        range_test_state = (vmm_guest_test_state_t){
            .address_space = other_root,
            .address = range.base
        };
        spinlock_init(&range_test_state.pin_lock);
        worker = thread_create_suspended("vmm-range-shootdown",
                                        vmm_guest_test_worker,
                                        &range_test_state);
        if (!worker)
            goto cleanup;
        if (!scheduler_enqueue(worker)) {
            (void)thread_destroy(worker);
            worker = 0;
            goto cleanup;
        }
        worker_started = true;
        if (!vmm_guest_wait(&range_test_state.ready) ||
            __atomic_load_n(&range_test_state.failed, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&range_test_state.cpu, __ATOMIC_ACQUIRE) != 1U)
            goto cleanup;
    }

#ifdef SLAB_CORE_GUEST_TEST
    if (!vmm_guest_test_slab_reclamation(online))
        goto cleanup;
#endif

    /* A copied token must not release a frame while its kernel-range PTE is
     * live. This check happens while CPU 1 is using a different CR3. */
    premature_release = frame;
    release_blocked = !pmm_owned_frame_release(&premature_release);
    if (!release_blocked || premature_release.physical != frame_physical)
        goto cleanup;

    if (!vmm_kernel_range_unmap(&range, 0U, 1U))
        goto cleanup;
    mapped = false;
    for (u32 index = 0; index < cpu_count(); index++) {
        cpu_local_t *cpu = cpu_by_index(index);
        if (cpu && __atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE))
            expected[index / 64U] |= 1ULL << (index % 64U);
    }
    vmm_kernel_range_guest_test_copy_shootdown_masks(
        acknowledgements, other_cr3, (CPU_MAX_COUNT + 63U) / 64U);
    for (u32 word = 0; word < (CPU_MAX_COUNT + 63U) / 64U; word++)
        if ((acknowledgements[word] & expected[word]) != expected[word])
            goto cleanup;
    if (online >= 2U &&
        !(other_cr3[1U / 64U] & (1ULL << (1U % 64U))))
        goto cleanup;

    if (!pmm_owned_frame_release(&frame) || frame.physical)
        goto cleanup;
    if (pmm_owned_frame_release(&premature_release))
        goto cleanup;
    reused_frame = pmm_alloc_frame();
    frame_reused = reused_frame == frame_physical;
    if (!frame_reused)
        goto cleanup;
    volatile const u64 *reused_words =
        (volatile const u64 *)phys_to_virt(reused_frame);
    if (!reused_words)
        goto cleanup;
    for (u32 word = 0; word < PAGE_SIZE / sizeof(u64); word++)
        if (reused_words[word] != 0U)
            goto cleanup;
    pmm_free_frame(reused_frame);
    reused_frame = 0;

    if (worker_started) {
        __atomic_store_n(&range_test_state.release, 1U, __ATOMIC_RELEASE);
        if (!vmm_guest_wait(&range_test_state.done) ||
            __atomic_load_n(&range_test_state.failed, __ATOMIC_ACQUIRE))
            goto cleanup;
        for (u64 spin = 0; spin < VMM_GUEST_TEST_SPINS; spin++) {
            if (worker->state == THREAD_STATE_TERMINATED &&
                !scheduler_thread_is_running(worker))
                break;
            (void)scheduler_yield();
        }
        if (worker->state != THREAD_STATE_TERMINATED ||
            scheduler_thread_is_running(worker))
            goto cleanup;
        if (!thread_destroy(worker))
            goto cleanup;
        worker = 0;
        worker_started = false;
    }
    if (!vmm_kernel_range_release(&range))
        goto cleanup;
    if (other_root) {
        vmm_destroy_address_space(other_root);
        other_root = 0;
    }
    result = true;

cleanup:
    if (worker_started) {
        __atomic_store_n(&range_test_state.release, 1U, __ATOMIC_RELEASE);
        if (!vmm_guest_wait(&range_test_state.done)) {
            kprint("VMM_RANGE_GUEST cleanup incomplete: worker did not stop\n");
            return false;
        }
        for (u64 spin = 0; spin < VMM_GUEST_TEST_SPINS; spin++) {
            if (worker->state == THREAD_STATE_TERMINATED &&
                !scheduler_thread_is_running(worker))
                break;
            (void)scheduler_yield();
        }
        if (worker->state != THREAD_STATE_TERMINATED ||
            scheduler_thread_is_running(worker)) {
            kprint("VMM_RANGE_GUEST cleanup incomplete: worker still owned\n");
            return false;
        }
        (void)thread_destroy(worker);
        worker_started = false;
    }
    if (mapped && vmm_kernel_range_unmap(&range, 0U, 1U))
        mapped = false;
    if (!mapped && range.generation)
        (void)vmm_kernel_range_release(&range);
    if (other_root)
        vmm_destroy_address_space(other_root);
    if (!mapped && frame.physical)
        (void)pmm_owned_frame_release(&frame);
    if (reused_frame)
        pmm_free_frame(reused_frame);
    if (result) {
        u64 ack_low = acknowledgements[0];
        u64 other_low = other_cr3[0];
        kprint("VMM_RANGE_GUEST pass=1 online=%u ack=%llx other_cr3=%llx release_blocked=%u frame_reused=%u\n",
               online, ack_low, other_low, release_blocked, frame_reused);
    } else {
        kprint("VMM_RANGE_GUEST pass=0 online=%u ready=%u failed=%u\n",
               online,
               __atomic_load_n(&range_test_state.ready, __ATOMIC_ACQUIRE),
               __atomic_load_n(&range_test_state.failed, __ATOMIC_ACQUIRE));
    }
    return result;
}
#endif
