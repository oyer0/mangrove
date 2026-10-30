/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <process.h>
#include <process_cleanup.h>
#include <scheduler.h>
#include <heap.h>
#include <string.h>
#include <vmm.h>
#include <pmm.h>
#include <elf_loader.h>
#include <vfs.h>
#include <mangrove_errors.h>
#include <kprint.h>
#include <console.h>
#include <terminal.h>
#include <service.h>
#include <session.h>
#include <ipc.h>
#include <mg/service.h>
#include <mg/process_limits.h>
#include <spinlock.h>
#include <panic.h>
#include <timer.h>
#include <kernel_stack.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define PROCESS_COMMAND_LINE_CAPACITY 256U

static void process_string_release(char **text)
{
    if (text && *text) {
        kfree(*text);
        *text = NULL;
    }
}

#define PROCESS_STRING_AUTO __attribute__((cleanup(process_string_release)))

typedef struct process_spawn_object_refs {
    kernel_object_t *console;
    kernel_object_t *separate_output;
} process_spawn_object_refs_t;

static void process_spawn_object_refs_release(process_spawn_object_refs_t *refs)
{
    if (!refs) return;
    if (refs->separate_output) object_release(refs->separate_output);
    if (refs->console) object_release(refs->console);
}

#define PROCESS_SPAWN_OBJECT_REFS_AUTO \
    __attribute__((cleanup(process_spawn_object_refs_release)))

static u64 next_pid;
static process_t *all_processes;
static spinlock_t process_registry_lock;
static bool process_shutdown_active;
/* Process-wide resource teardown runs on this ordinary kernel thread, never
 * in the assembly context-switch callback. Pending work is derived by scanning
 * all_processes; this is one waiter, not a dynamically growing work queue. */
static kernel_thread_t *process_cleanup_worker;
/* Coalesced request to retry collection after a process reference has been
 * released from a context where session collection is unsafe. The worker
 * discovers candidates from all_processes, so the flag retains no object or
 * session pointer and cannot create a reference cycle. */
static _Atomic bool process_session_collection_retry_pending;
static _Atomic bool process_cleanup_worker_ready;
static _Atomic u32 process_cleanup_work_pending;
static _Atomic u64 process_worker_reap_generation;
/* Only the cleanup worker owns these scalar sweep cursors. */
static u64 process_worker_reap_after, process_worker_reap_limit;
static u64 process_worker_reap_seen_generation;
static bool process_worker_reap_sweep_active;


#define PROCESS_SHUTDOWN_GRACE_MS 5000U
#define PROCESS_SHUTDOWN_FORCE_MS 5000U

extern void ring3_enter(uintptr_t entry, uintptr_t stack_pointer,
                        uintptr_t arg0, uintptr_t arg1,
                        uintptr_t fs_base) __attribute__((noreturn));

typedef struct {
    kernel_object_t *object;
    u32 rights;
    u16 generation;
    bool active;
    bool reserved;
} process_handle_slot_t;

typedef struct process_memory_mapping {
    uintptr_t address;
    usize page_count;
    struct process_memory_mapping *next;
} process_memory_mapping_t;

#define PROCESS_USER_THREAD_RETURN_ADDRESS ((uintptr_t)0x00400000U)
#define PROCESS_TLS_MIN_ALIGNMENT 1U
#define PROCESS_TLS_TCB_BYTES (2U * sizeof(uintptr_t))

typedef enum {
    PROCESS_REGION_RELEASE_INVALID = 0,
    PROCESS_REGION_RELEASE_DEFERRED,
    PROCESS_REGION_RELEASED,
} process_region_release_result_t;

static bool process_size_align_up(usize value, usize alignment,
                                  usize *out_value)
{
    usize mask;
    if (!alignment || (alignment & (alignment - 1U)) || !out_value)
        return false;
    mask = alignment - 1U;
    if (value > ~(usize)0 - mask) return false;
    *out_value = (value + mask) & ~mask;
    return true;
}

static uintptr_t process_address_align_up(uintptr_t value, usize alignment,
                                         bool *ok)
{
    uintptr_t mask = (uintptr_t)alignment - 1U;
    if (!alignment || (alignment & (alignment - 1U)) ||
        value > ~(uintptr_t)0 - mask) {
        if (ok) *ok = false;
        return 0;
    }
    if (ok) *ok = true;
    return (value + mask) & ~mask;
}

#define PROCESS_HANDLE_SLOTS_INITIAL 16U
#define HANDLE_INDEX_MASK 0xffffU

static void process_memory_release_all(process_t *process);
static void process_user_thread_entry(void *argument);
static void process_user_region_metadata_release(process_t *process);
static void process_unlink(process_t *process);
static void process_unlink_all(process_t *process);
static void process_unlink_locked(process_t *process);
static void process_unlink_all_locked(process_t *process);
static bool process_cleanup_exit_resources(process_t *process);
static bool process_finish_termination(process_t *process);
static bool process_cleanup_context_valid(void);
static bool process_publish_terminated_locked(process_t *process);
static void process_waiter_wake_locked(process_t *process,
                                        kernel_thread_t *waiter);
static void process_worker_records_release_all(process_t *process);
static void process_cleanup_worker_entry(void *unused);
static bool process_cleanup_worker_wake(process_t *hint);
static bool process_cleanup_worker_retry_session_collections_once(void);
static void process_cleanup_worker_request_session_collection_retry(void);
static kernel_object_t *process_handle_lookup_reference(
    process_t *process, process_handle_t handle,
    kernel_object_type_t type, u32 required_rights);
/* Waiting has the same check-to-sleep handoff as IPC.  The registry lock is
 * released before scheduler_block(), but local interrupts remain masked until
 * the waiter has been registered or the failed handoff has been cleaned up. */
static u64 process_cli_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void process_cli_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

static process_handle_slot_t *handle_slots(process_t *process)
{
    return (process_handle_slot_t *)process->handle_table;
}

/* Keep low-16-bit handle indices representable: the encoded slot is index+1,
 * and zero remains the invalid handle value. */
#define PROCESS_HANDLE_SLOTS_MAX HANDLE_INDEX_MASK

static void process_handle_revision_advance(process_t *process)
{
    process->handle_revision++;
    if (!process->handle_revision) process->handle_revision = 1U;
}

static bool process_handle_grow(process_t *process)
{
    bool success = false;

    if (!process || !mutex_lock(&process->handle_growth_mutex)) return false;
    for (;;) {
        process_handle_slot_t *old_slots;
        process_handle_slot_t *new_slots;
        usize old_capacity, new_capacity;
        u64 old_revision, flags;

        flags = spin_lock_irqsave(&process->handle_lock);
        old_capacity = process->handle_capacity;
        old_slots = handle_slots(process);
        old_revision = process->handle_revision;
        spin_unlock_irqrestore(&process->handle_lock, flags);
        if (!old_capacity || old_capacity >= PROCESS_HANDLE_SLOTS_MAX ||
            !old_slots) break;
        new_capacity = old_capacity > PROCESS_HANDLE_SLOTS_MAX / 2U
            ? PROCESS_HANDLE_SLOTS_MAX : old_capacity * 2U;
        if (new_capacity > (usize)-1 / sizeof(*new_slots)) break;
        new_slots = kmalloc(new_capacity * sizeof(*new_slots));
        if (!new_slots) break;
        /* Every slot writer holds handle_growth_mutex. The slot lock protects
         * pointer publication and readers, while this mutex makes the copy a
         * stable snapshot without keeping interrupts disabled for the copy. */
        memcpy(new_slots, old_slots, old_capacity * sizeof(*new_slots));
        memset(new_slots + old_capacity, 0,
               (new_capacity - old_capacity) * sizeof(*new_slots));

        flags = spin_lock_irqsave(&process->handle_lock);
        if (process->handle_table != old_slots ||
            process->handle_capacity != old_capacity ||
            process->handle_revision != old_revision) {
            spin_unlock_irqrestore(&process->handle_lock, flags);
            kfree(new_slots);
            continue;
        }
        process->handle_table = new_slots;
        process->handle_capacity = new_capacity;
        process_handle_revision_advance(process);
        spin_unlock_irqrestore(&process->handle_lock, flags);
        kfree(old_slots);
        success = true;
        break;
    }
    (void)mutex_unlock(&process->handle_growth_mutex);
    return success;
}

static process_handle_t encode_handle(u32 index, u16 generation)
{
    return ((process_handle_t)generation << 16) | (index + 1U);
}

static bool decode_handle(process_handle_t handle, u32 *index, u16 *generation)
{
    u32 slot = handle & HANDLE_INDEX_MASK;
    if (!slot || !index || !generation) return false;
    *index = slot - 1U;
    *generation = (u16)(handle >> 16);
    return *generation != 0;
}

static process_t *object_process(kernel_object_t *object)
{
    return (process_t *)((u8 *)object - __builtin_offsetof(process_t, object));
}

static vfs_location_t *process_cwd_acquire(process_t *process)
{
    if (!process) return NULL;
    u64 flags = spin_lock_irqsave(&process->cwd_lock);
    vfs_location_t *location = process->cwd_location;
    if (location && !vfs_location_retain(location)) location = NULL;
    spin_unlock_irqrestore(&process->cwd_lock, flags);
    return location;
}

static void process_cwd_clear(process_t *process)
{
    u64 flags = spin_lock_irqsave(&process->cwd_lock);
    vfs_location_t *location = process->cwd_location;
    process->cwd_location = NULL;
    spin_unlock_irqrestore(&process->cwd_lock, flags);
    vfs_location_cwd_release(location);
}

static void process_object_destroy(kernel_object_t *object)
{
    process_t *process = object_process(object);
    process_t *child;
    process_t *next;

    if (process->active_syscalls || process->live_thread_count ||
        process->thread_list_readers ||
        process->thread_count || process->threads || process->main_thread ||
        process->worker_reapers || process->thread_records ||
        process->scheduler_handoff_references ||
        __atomic_load_n(&process->scheduler_handoff_releases_pending,
                        __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&process->deferred_reference_releases_pending,
                        __ATOMIC_ACQUIRE) ||
        process->deferred_reference_collection_retry)
        panic("process: destruction with owned thread state");

    process_cwd_clear(process);
    process_unlink_all(process);
    /* A parent may be reaped before a detached/session child.  Never leave
     * children pointing into the soon-to-be-freed process object. */
    {
        u64 flags = spin_lock_irqsave(&process_registry_lock);
        child = process->first_child;
        while (child) {
            next = child->next_sibling;
            child->parent = NULL;
            child->next_sibling = NULL;
            child = next;
        }
        process->first_child = NULL;
        spin_unlock_irqrestore(&process_registry_lock, flags);
    }
    process_memory_release_all(process);
    vmm_destroy_address_space(process->address_space);
    process_user_region_metadata_release(process);
    kfree(process->handle_table);
    kfree(process);
}

bool process_init(void)
{
    spinlock_init(&process_registry_lock);
    next_pid = 1;
    all_processes = NULL;
    process_shutdown_active = false;
    process_cleanup_work_pending = 0;
    process_worker_reap_generation = 0;
    process_worker_reap_after = process_worker_reap_limit = 0;
    process_worker_reap_sweep_active = false;
    process_cleanup_worker = NULL;
    __atomic_store_n(&process_session_collection_retry_pending, false,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&process_cleanup_worker_ready, false,
                     __ATOMIC_RELAXED);
    return true;
}

/* Caller holds process_registry_lock. The process list is also the lifetime
 * owner for this scheduler record; one process-object reference pins its
 * address space and shared resources until the record is detached. */
static bool process_thread_register_locked(process_t *process,
                                           kernel_thread_t *thread,
                                           bool allow_bootstrap_current)
{
    if (!process || !thread || process->state != PROCESS_STATE_ACTIVE ||
        __atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE) ||
        process->termination_pending || thread->process ||
        thread->process_state != THREAD_PROCESS_UNATTACHED ||
        thread->process_reference_held ||
        process->thread_count == ~(u32)0 ||
        process->live_thread_count > process->thread_count ||
        process->live_thread_count == ~(u32)0 ||
        (!scheduler_thread_process_attachable(thread) &&
         !(allow_bootstrap_current &&
           scheduler_thread_initial_process_attachable(thread))) ||
        (!process->thread_count && (process->threads || process->main_thread ||
                                    process->live_thread_count)) ||
        (process->thread_count && !process->main_thread) ||
        !object_reference(&process->object))
        return false;

    thread->process = process;
    thread->process_next = process->threads;
    thread->process_state = THREAD_PROCESS_ATTACHED;
    thread->process_initial = process->main_thread == NULL;
    thread->process_reference_held = true;
    __atomic_store_n(&thread->process_exit_requested, false,
                     __ATOMIC_RELAXED);
    thread->process_exit_status = 0;
    thread->process_exit_status_valid = false;
    process->threads = thread;
    process->thread_count++;
    process->live_thread_count++;
    if (thread->process_initial) process->main_thread = thread;
    return true;
}

static bool process_thread_list_complete_locked(process_t *process)
{
    u32 count = 0;
    bool main_seen = process && !process->main_thread;

    if (!process || !process->thread_count || process->live_thread_count ||
        process->active_syscalls || process->worker_reapers)
        return false;
    for (kernel_thread_t *thread = process->threads; thread;
         thread = thread->process_next) {
        /* Bound traversal by the published count. This also rejects a
         * malformed cycle instead of walking it indefinitely. */
        if (count >= process->thread_count || thread->process != process ||
            !thread->process_reference_held ||
            thread->process_state != THREAD_PROCESS_HANDOFF_COMPLETE ||
            !scheduler_thread_exit_complete(thread))
            return false;
        count++;
        if (thread->process_initial && process->main_thread != thread)
            return false;
        if (thread == process->main_thread) {
            if (!thread->process_initial) return false;
            main_seen = true;
        }
    }
    return count == process->thread_count && main_seen;
}

/* Caller holds process_registry_lock. Validate a scheduler descriptor's
 * process-list ownership without exporting a raw pointer past the lock. Bound
 * the walk by the published count so a corrupt cycle cannot hang completion
 * delivery in scheduler context. */
static bool process_thread_member_locked(process_t *process,
                                        kernel_thread_t *target)
{
    u32 count = 0;
    bool found = false;

    if (!process || !target || !process->thread_count || !process->threads)
        return false;
    for (kernel_thread_t *thread = process->threads; thread;
         thread = thread->process_next) {
        if (count >= process->thread_count || thread->process != process ||
            !thread->process_reference_held)
            return false;
        if (thread == target) found = true;
        count++;
    }
    return count == process->thread_count && found;
}

/* Session collection may race a temporary object pin after the process's
 * final thread list has already been detached. That state is still a valid
 * collection candidate once the finalizer has published its completion. */
static bool process_session_collection_ready_locked(process_t *process)
{
    if (!process) return false;
    if (process->thread_count)
        return process_thread_list_complete_locked(process);

    return process->state == PROCESS_STATE_TERMINATED &&
        process->exit_cleanup_complete && process->exit_notification_sent &&
        !process->exit_cleanup_in_progress && !process->active_syscalls &&
        !process->live_thread_count && !process->threads &&
        !process->main_thread && !process->thread_records &&
        !process->thread_list_readers && !process->thread_reap_in_progress &&
        !process->worker_reapers && !process->wait_claimed;
}

static process_t *process_cleanup_candidate_locked(void)
{
    for (process_t *process = all_processes; process;
         process = process->next_all) {
        if (!process->launch_ready ||
            process->state != PROCESS_STATE_EXITING ||
            process->exit_cleanup_complete ||
            process->exit_cleanup_in_progress ||
            !process_thread_list_complete_locked(process))
            continue;
        if (object_reference(&process->object))
            return process;
    }
    return NULL;
}

#define PROCESS_HANDOFF_RELEASE_BATCH 16U

/* Scheduler callbacks transfer their retained process reference to this
 * worker instead of dropping it on an incoming stack with a pending mutex
 * handoff. A bounded batch keeps each pass finite if several CPUs publish at
 * once. The reference registry is the work list: each pending reference keeps
 * its process linked until this worker releases it. */
static bool process_cleanup_worker_release_handoffs_once(void)
{
    process_t *process = NULL;
    u32 release_count = 0;
    mg_session_id_t session_id = PROCESS_NO_SESSION;
    u64 flags = spin_lock_irqsave(&process_registry_lock);

    for (process_t *candidate = all_processes; candidate;
         candidate = candidate->next_all) {
        u32 pending = __atomic_load_n(
            &candidate->scheduler_handoff_releases_pending,
            __ATOMIC_ACQUIRE);
        if (!pending) continue;
        if (!object_reference(&candidate->object)) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: pending scheduler reference lost its owner");
        }
        release_count = pending < PROCESS_HANDOFF_RELEASE_BATCH
            ? pending : PROCESS_HANDOFF_RELEASE_BATCH;
        pending = __atomic_fetch_sub(
            &candidate->scheduler_handoff_releases_pending, release_count,
            __ATOMIC_ACQ_REL);
        if (pending < release_count) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: scheduler release publication underflow");
        }
        if (candidate->scheduler_handoff_references < release_count ||
            __atomic_load_n(&candidate->object.ref_count,
                            __ATOMIC_ACQUIRE) <= release_count) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: scheduler handoff ownership accounting lost");
        }
        candidate->scheduler_handoff_references -= release_count;
        /* object_reference() above leaves a worker pin in place. Releasing
         * only the claimed tokens cannot destroy the process here; doing the
         * atomic drops under the registry lock keeps session-reap reference
         * accounting coherent at every lock boundary. */
        for (u32 i = 0; i < release_count; i++)
            object_release(&candidate->object);
        process = candidate;
        session_id = candidate->session_id;
        break;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (!process) return false;

    /* This may now be the final reference. Process destruction is permitted
     * here: this is the ordinary cleanup worker, not a switch callback. */
    object_release(&process->object);

    /* A concurrent session reap can decline while this worker holds its
     * temporary scan reference. Retry after dropping that pin so the deferred
     * release cannot strand an otherwise collectable process. */
    if (session_id != PROCESS_NO_SESSION)
        process_reap_session_members(session_id);
    return true;
}

/* Transfer a retained process reference to the cleanup worker when dropping
 * it in the caller could run process destruction in an unsafe context. The
 * pending count is protected by process_registry_lock and each unit continues
 * to own its original object reference. A worker pin protects the process
 * while a bounded batch is detached and released outside the registry lock. */
#define PROCESS_DEFERRED_REFERENCE_RELEASE_BATCH 16U
static bool process_cleanup_worker_release_deferred_references_once(void)
{
    process_t *process = NULL;
    u32 release_count = 0;
    mg_session_id_t retry_session = PROCESS_NO_SESSION;
    u64 flags = spin_lock_irqsave(&process_registry_lock);

    for (process_t *candidate = all_processes; candidate;
         candidate = candidate->next_all) {
        u32 pending = __atomic_load_n(
            &candidate->deferred_reference_releases_pending,
            __ATOMIC_ACQUIRE);
        u32 remaining;

        if (!pending) continue;
        if (!object_reference(&candidate->object)) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: deferred reference lost its process");
        }
        release_count = pending < PROCESS_DEFERRED_REFERENCE_RELEASE_BATCH
            ? pending : PROCESS_DEFERRED_REFERENCE_RELEASE_BATCH;
        pending = __atomic_fetch_sub(
            &candidate->deferred_reference_releases_pending, release_count,
            __ATOMIC_ACQ_REL);
        if (pending < release_count) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: deferred reference count underflow");
        }
        remaining = pending - release_count;
        if (__atomic_load_n(&candidate->object.ref_count,
                            __ATOMIC_ACQUIRE) <= release_count) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: deferred reference ownership lost");
        }

        /* Publishers and this worker serialize on the registry lock. Keep the
         * retry marker until the last queued reference is claimed, so a
         * bounded batch cannot consume the only collection retry. */
        if (!remaining && candidate->deferred_reference_collection_retry) {
            retry_session = candidate->session_id;
            candidate->deferred_reference_collection_retry = false;
        }
        for (u32 i = 0; i < release_count; i++)
            object_release(&candidate->object);
        process = candidate;
        break;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (!process) return false;

    /* The worker pin is now the only reference needed to safely finish this
     * batch. Its release may destroy the process, so retain only the scalar
     * session identity beyond this point. */
    object_release(&process->object);
    if (retry_session != PROCESS_NO_SESSION)
        process_reap_session_members(retry_session);
    return true;
}

/* A single atomic latch coalesces cleanup-worker passes needed by transferred
 * references and session-collection retries. The process registry is the
 * candidate list; scan by monotonically allocated session ID and retain only
 * the scalar ID across the unlocked reaper call. */
static bool process_cleanup_worker_retry_session_collections_once(void)
{
    if (!__atomic_exchange_n(&process_session_collection_retry_pending, false,
                             __ATOMIC_ACQ_REL))
        return false;

    mg_session_id_t after = PROCESS_NO_SESSION;
    for (;;) {
        mg_session_id_t next_session = PROCESS_NO_SESSION;
        u64 flags = spin_lock_irqsave(&process_registry_lock);
        for (process_t *process = all_processes; process;
             process = process->next_all) {
            mg_session_id_t candidate = process->session_id;
            if (process->launch_ready && process->owner_reference_held &&
                process->state == PROCESS_STATE_TERMINATED &&
                candidate > after &&
                (!next_session || candidate < next_session))
                next_session = candidate;
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (next_session == PROCESS_NO_SESSION) break;
        process_reap_session_members(next_session);
        after = next_session;
    }
    return true;
}

static void process_cleanup_worker_request_session_collection_retry(void)
{
    bool already_pending = __atomic_exchange_n(
        &process_session_collection_retry_pending, true, __ATOMIC_SEQ_CST);
    if (already_pending) return;

    /* During worker startup, the durable latch is enough: startup checks it
     * after enqueue. Once ready, every publication must either wake the
     * blocked worker or latch a wake while it is running. */
    if (__atomic_load_n(&process_cleanup_worker_ready, __ATOMIC_SEQ_CST) &&
        !process_cleanup_worker_wake(NULL))
        panic("process: collection retry worker could not be notified");
}

void process_cleanup_request(unsigned int work)
{
    if (work & PROCESS_CLEANUP_WORKER_REAP) {
        if (__atomic_fetch_add(&process_worker_reap_generation, 1U,
                               __ATOMIC_RELEASE) == ~(u64)0)
            panic("process: cleanup generation overflow");
    }
    /* Startup publishes ready then inspects work; producers publish work
     * then inspect ready. A single SC order prevents both sides observing
     * the old value and leaving pre-start work unnotified. */
    __atomic_fetch_or(&process_cleanup_work_pending, work, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&process_cleanup_worker_ready, __ATOMIC_SEQ_CST))
        (void)process_cleanup_worker_wake(NULL);
}

/* A finite PID sweep tries each current process once. Deferred leases/readers
 * do not requeue themselves; a real dependency transition publishes work.
 * New progress during a sweep starts one subsequent sweep, not a busy poll. */
static bool process_cleanup_worker_reap_once(void)
{
    bool worked = false;
    if (!process_worker_reap_sweep_active) {
        process_worker_reap_seen_generation = __atomic_load_n(
            &process_worker_reap_generation, __ATOMIC_ACQUIRE);
        u64 flags = spin_lock_irqsave(&process_registry_lock);
        process_worker_reap_limit = 0;
        for (process_t *p = all_processes; p; p = p->next_all)
            if (p->pid > process_worker_reap_limit)
                process_worker_reap_limit = p->pid;
        spin_unlock_irqrestore(&process_registry_lock, flags);
        process_worker_reap_after = 0;
        process_worker_reap_sweep_active = true;
    }
    for (u32 count = 0; count < 16U; count++) {
        process_t *candidate = NULL;
        u64 flags = spin_lock_irqsave(&process_registry_lock);
        for (process_t *p = all_processes; p; p = p->next_all) {
            if (p->pid <= process_worker_reap_after ||
                p->pid > process_worker_reap_limit ||
                (candidate && p->pid >= candidate->pid) ||
                p->state != PROCESS_STATE_ACTIVE || p->termination_pending ||
                p->worker_reapers ||
                (!__atomic_load_n(&p->worker_reap_pending, __ATOMIC_ACQUIRE) &&
                 !__atomic_load_n(&p->user_region_reap_pending, __ATOMIC_ACQUIRE)))
                continue;
            candidate = p;
        }
        if (candidate && !object_reference(&candidate->object))
            panic("process: cleanup sweep lost process owner");
        if (candidate) process_worker_reap_after = candidate->pid;
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!candidate) {
            process_worker_reap_sweep_active = false;
            if (__atomic_load_n(&process_worker_reap_generation,
                                 __ATOMIC_ACQUIRE) !=
                    process_worker_reap_seen_generation)
                __atomic_fetch_or(&process_cleanup_work_pending,
                    PROCESS_CLEANUP_WORKER_REAP, __ATOMIC_RELEASE);
            return worked;
        }
        process_worker_reclaim_pending(candidate);
        object_release(&candidate->object);
        worked = true;
    }
    /* Continue only to visit the remaining finite snapshot, not to retry a
     * process already attempted in this generation. */
    __atomic_fetch_or(&process_cleanup_work_pending,
                     PROCESS_CLEANUP_WORKER_REAP, __ATOMIC_RELEASE);
    return worked;
}

static bool process_cleanup_worker_run_once(void)
{
    process_t *process;
    u32 work = __atomic_exchange_n(&process_cleanup_work_pending, 0U,
                                    __ATOMIC_ACQ_REL);
    bool worked = process_cleanup_worker_release_handoffs_once();
    if ((work & PROCESS_CLEANUP_WORKER_REAP) ||
        process_worker_reap_sweep_active) {
        if (process_cleanup_worker_reap_once()) worked = true;
    }
    if (process_cleanup_worker_release_deferred_references_once())
        worked = true;
    mg_session_id_t session_id;
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    process = process_cleanup_candidate_locked();
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (process) {
        session_id = process->session_id;
        (void)process_cleanup_exit_resources(process);
        object_release(&process->object);
        if (session_id != PROCESS_NO_SESSION)
            process_reap_session_members(session_id);
        worked = true;
    }

    if (process_cleanup_worker_retry_session_collections_once())
        worked = true;
    /* A finite sweep can publish its own continuation without an external
     * scheduler wake. Consume that work before considering a park. */
    return worked || __atomic_load_n(&process_cleanup_work_pending,
                                     __ATOMIC_ACQUIRE) != 0U;
}

static void process_cleanup_worker_entry(void *unused)
{
    (void)unused;
    for (;;) {
        kernel_thread_t *self = thread_current();
        scheduler_clear_event_wake(self);
        if (process_cleanup_worker_run_once())
            continue;
        /* scheduler_wake_waiter latches an event when this thread is still
         * RUNNING, so a state change between the registry scan and this park
         * cannot be lost. */
        (void)scheduler_block();
    }
}

static bool process_cleanup_worker_wake(process_t *hint)
{
    (void)hint; /* Work is discovered from the authoritative process registry. */
    kernel_thread_t *worker = __atomic_load_n(&process_cleanup_worker,
                                               __ATOMIC_ACQUIRE);
    if (!worker) return false;
    if (!scheduler_wake_waiter(worker))
        panic("process: cleanup worker could not be notified");
    return true;
}

bool process_start_cleanup_worker(void)
{
    kernel_thread_t *worker = thread_create_suspended(
        "process-cleanup", process_cleanup_worker_entry, NULL);
    u64 flags;

    if (!worker) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (__atomic_load_n(&process_cleanup_worker, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!scheduler_terminate_thread(worker) || !thread_destroy(worker))
            panic("process: duplicate cleanup worker rollback failed");
        return false;
    }
    __atomic_store_n(&process_cleanup_worker, worker, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&process_registry_lock, flags);

    if (scheduler_enqueue(worker)) {
        __atomic_store_n(&process_cleanup_worker_ready, true,
                         __ATOMIC_SEQ_CST);
        if ((__atomic_load_n(&process_session_collection_retry_pending,
                            __ATOMIC_SEQ_CST) ||
             __atomic_load_n(&process_cleanup_work_pending, __ATOMIC_SEQ_CST)) &&
            !process_cleanup_worker_wake(NULL))
            panic("process: startup collection retry wake failed");
        return true;
    }

    flags = spin_lock_irqsave(&process_registry_lock);
    __atomic_store_n(&process_cleanup_worker, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&process_cleanup_worker_ready, false,
                     __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (!scheduler_terminate_thread(worker) || !thread_destroy(worker))
        panic("process: cleanup worker startup rollback failed");
    return false;
}

static void process_thread_record_destroy(kernel_object_t *object)
{
    process_thread_record_t *record;
    if (!object) return;
    record = (process_thread_record_t *)((u8 *)object -
        __builtin_offsetof(process_thread_record_t, object));
    if (record->listed || record->reclaiming || record->execution ||
        record->stack_region || record->tls_region)
        panic("process: destroying live worker completion record");
    kfree(record);
}

static void process_worker_records_release_all(process_t *process)
{
    process_thread_record_t *records;
    u64 flags;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    records = process->thread_records;
    process->thread_records = NULL;
    __atomic_store_n(&process->worker_reap_pending, false,
                     __ATOMIC_RELEASE);
    for (process_thread_record_t *record = records; record;
         record = record->next) {
        record->listed = false;
        record->process = NULL;
        record->execution = NULL;
        /* The whole address space is reclaimed by process destruction. */
        record->stack_region = NULL;
        record->tls_region = NULL;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);

    while (records) {
        process_thread_record_t *next = records->next;
        records->next = NULL;
        object_release(&records->object); /* process-list ownership */
        records = next;
    }
}

/* Caller holds process_registry_lock and an object reference. This short
 * reader pin lets callers walk the intrusive list without keeping the
 * registry lock across scheduler wake operations. Final reap closes admission
 * atomically with checking the count. */
static bool process_thread_list_read_acquire_locked(process_t *process)
{
    if (!process || process->state == PROCESS_STATE_TERMINATED ||
        process->thread_reap_in_progress || !process->threads ||
        !process->thread_count ||
        process->thread_list_readers == ~(u32)0)
        return false;
    process->thread_list_readers++;
    return true;
}

static void process_thread_list_read_release(process_t *process)
{
    u64 flags;
    bool queue_cleanup;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (!process->thread_list_readers) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        panic("process: thread-list reader underflow");
    }
    process->thread_list_readers--;
    bool finish_exit = !process->thread_list_readers &&
        process->state == PROCESS_STATE_EXITING &&
        process->exit_cleanup_complete;
    queue_cleanup = process->state == PROCESS_STATE_EXITING &&
        !process->exit_cleanup_complete;
    bool reap_workers = !process->thread_list_readers &&
        process->state == PROCESS_STATE_ACTIVE &&
        __atomic_load_n(&process->worker_reap_pending, __ATOMIC_ACQUIRE);
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (reap_workers) process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);
    if (queue_cleanup) process_cleanup_worker_wake(process);
    if (finish_exit) (void)process_finish_termination(process);
}

/* Caller holds process_registry_lock. Registration cancellation, validation
 * and scheduler publication must be one transaction; a retained descriptor
 * alone would permit a stale wake after the caller starts a different wait. */
static void process_waiter_wake_locked(process_t *process,
                                       kernel_thread_t *waiter)
{
    if (!process || !waiter || !waiter->process_reference_held ||
        waiter->process != process ||
        (waiter->process_state != THREAD_PROCESS_ATTACHED &&
         waiter->process_state != THREAD_PROCESS_EXIT_REQUESTED &&
         waiter->process_state != THREAD_PROCESS_HANDOFF_COMPLETE) ||
        !process_thread_member_locked(process, waiter))
        panic("process: child waiter lost process ownership");

    scheduler_wake_result_t result = scheduler_wake_waiter_result(waiter);
    if (result != SCHEDULER_WAKE_DELIVERED &&
        result != SCHEDULER_WAKE_ALREADY_TERMINATED)
        panic("process: live child waiter could not be woken");
}

/* Caller holds process_registry_lock.  Revoking this state at the first
 * process-exit transition prevents an admitted sibling syscall from using a
 * grant after process-wide teardown has begun. */
static void process_storage_authorization_clear_locked(process_t *process)
{
    if (!process) return;
    process->storage_authorized_operation = 0U;
    process->storage_authorized_thread_id = 0U;
    process->storage_management_session = false;
    process->storage_management_authorizing = false;
}

/* A worker may leave the process active. Do not strand a one-shot grant on
 * the exited thread identity, where no sibling can consume or cancel it. */
static void process_storage_authorization_revoke_thread_locked(
    process_t *process, u64 thread_id)
{
    if (process && thread_id &&
        process->storage_authorized_thread_id == thread_id) {
        process->storage_authorized_operation = 0U;
        process->storage_authorized_thread_id = 0U;
    }
}

static bool process_publish_terminated_locked(process_t *process)
{
    if (!process || process->state != PROCESS_STATE_EXITING ||
        process->thread_list_readers ||
        !process->exit_cleanup_complete ||
        !process_thread_list_complete_locked(process))
        return false;

    process->state = PROCESS_STATE_TERMINATED;
    process->termination_pending = false;
    process->external_termination_in_progress = false;
    process_storage_authorization_clear_locked(process);
    if (process->parent && process->parent->state == PROCESS_STATE_ACTIVE &&
        process->parent->waiting_child == process &&
        process->parent->waiting_thread) {
        process_t *parent = process->parent;
        /* The registry excludes descriptor removal; the wait syscall retains
         * its registration reader and process admission until it unregisters.
         * Publish before clearing the exact registration, never after unlock. */
        process_waiter_wake_locked(parent, parent->waiting_thread);
        parent->waiting_child = NULL;
        parent->waiting_thread = NULL;
    }
    return true;
}

process_t *process_create_with_failure(const char *name, process_t *parent,
                                       struct kernel_thread *main_thread,
                                       const char **out_stage,
                                       int *out_status)
{
    process_t *process;
    int status = VFS_OK;

    if (out_stage) *out_stage = NULL;
    if (out_status) *out_status = VFS_OK;

    if (!name || !main_thread) {
        if (out_stage) *out_stage = "invalid input";
        return NULL;
    }

    process = (process_t *)kmalloc(sizeof(*process));
    if (!process) {
        if (out_stage) *out_stage = "process object allocation";
        return NULL;
    }
    memset(process, 0, sizeof(*process));
    spinlock_init(&process->handle_lock);
    mutex_init(&process->handle_growth_mutex);
    spinlock_init(&process->cwd_lock);
    mutex_init(&process->memory_lock);
    object_init(&process->object, OBJECT_TYPE_PROCESS,
                process_object_destroy);
    process->handle_capacity = PROCESS_HANDLE_SLOTS_INITIAL;
    process->handle_table = kmalloc(sizeof(process_handle_slot_t) *
                                    process->handle_capacity);
    if (!process->handle_table) {
        if (out_stage) *out_stage = "handle table allocation";
        kfree(process);
        return NULL;
    }
    memset(process->handle_table, 0,
           sizeof(process_handle_slot_t) * process->handle_capacity);
    process->address_space = vmm_create_address_space();
    if (!process->address_space) {
        if (out_stage) *out_stage = "address-space creation";
        kfree(process->handle_table);
        kfree(process);
        return NULL;
    }
    process->pid = 0;
    process->state = PROCESS_STATE_ACTIVE;
    process->parent = parent;
    process->credentials = identity_system_credentials();
    process->credentials_initialized = false;
    process->session_id = parent ? parent->session_id : PROCESS_NO_SESSION;
    process->session_shell = false;
    process->owner_reference_held = true;
    terminal_output_state_init(&process->terminal_output);
    process_credentials_t inherited_credentials;
    if (parent && process_get_credentials(parent, &inherited_credentials)) {
        process->credentials = inherited_credentials;
        process->credentials_initialized = true;
    }
    strncpy(process->name, name, sizeof(process->name) - 1);
    process->name[sizeof(process->name) - 1] = '\0';
    vfs_location_t *inherited = parent ? process_cwd_acquire(parent) : NULL;
    if (!parent) {
        status = vfs_location_lookup(NULL, "/", NULL, NULL, &inherited);
        if (status != VFS_OK) {
            if (out_stage) *out_stage = "root working-directory lookup";
            if (out_status) *out_status = status;
        }
    }
    if (!inherited || !(parent ? vfs_location_cwd_inherit(inherited)
                               : vfs_location_cwd_retain(inherited))) {
        if (out_stage && !*out_stage)
            *out_stage = parent ? "parent working-directory inheritance"
                                : "root working-directory retention";
        vfs_location_release(inherited);
        vmm_destroy_address_space(process->address_space);
        kfree(process->handle_table);
        kfree(process);
        return NULL;
    }
    process->cwd_location = inherited;
    vfs_location_release(inherited);

    {
        u64 flags = spin_lock_irqsave(&process_registry_lock);
        if (process_shutdown_active || next_pid == 0 || main_thread->process ||
            (parent && parent->state != PROCESS_STATE_ACTIVE)) {
            if (out_stage) *out_stage = process_shutdown_active
                ? "system shutdown in progress" : "process registry validation";
            spin_unlock_irqrestore(&process_registry_lock, flags);
            process_cwd_clear(process);
            vmm_destroy_address_space(process->address_space);
            kfree(process->handle_table);
            kfree(process);
            return NULL;
        }
        if (!process_thread_register_locked(process, main_thread, true)) {
            if (out_stage) *out_stage = "initial thread registration";
            spin_unlock_irqrestore(&process_registry_lock, flags);
            process_cwd_clear(process);
            vmm_destroy_address_space(process->address_space);
            kfree(process->handle_table);
            kfree(process);
            return NULL;
        }
        process->pid = next_pid++;
        if (parent) {
            process->next_sibling = parent->first_child;
            parent->first_child = process;
        }
        process->next_all = all_processes;
        all_processes = process;
        spin_unlock_irqrestore(&process_registry_lock, flags);
    }
    return process;
}

process_t *process_create(const char *name, process_t *parent,
                          struct kernel_thread *main_thread)
{
    return process_create_with_failure(name, parent, main_thread, NULL, NULL);
}

bool process_assign_system_credentials(process_t *process)
{
    u64 flags;
    if (!process) return false;
    process_credentials_t credentials = identity_system_credentials();
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state != PROCESS_STATE_ACTIVE || process->parent ||
        process->credentials_initialized) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    process->credentials = credentials;
    process->credentials_initialized = true;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return true;
}

bool process_assign_system_service(process_t *process, u32 service_id)
{
    const kernel_service_definition_t *definition;
    process_credentials_t credentials;
    u64 flags;

    if (!process || !service_definition_lookup(service_id, &definition) ||
        !definition)
        return false;
    credentials = identity_system_credentials();
    credentials.service_privileges = definition->privileges;
    if (!identity_credentials_valid(&credentials)) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state != PROCESS_STATE_ACTIVE || process->parent ||
        process->credentials_initialized) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    process->credentials = credentials;
    process->credentials_initialized = true;
    process->system_service = true;
    process->service_id = service_id;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return true;
}

bool process_get_credentials(const process_t *process,
                             process_credentials_t *credentials)
{
    process_credentials_t snapshot;
    u64 flags;

    if (!process || !credentials) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    bool available = process->state == PROCESS_STATE_ACTIVE &&
                     process->credentials_initialized;
    if (available) snapshot = process->credentials;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (!available || !identity_credentials_valid(&snapshot)) return false;
    return identity_credentials_effective(&snapshot, credentials);
}

bool process_attach_thread(process_t *process, struct kernel_thread *thread)
{
    bool attached;
    u64 flags;

    if (!process || !thread) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    attached = process_thread_register_locked(process, thread, false);
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return attached;
}

static bool process_detach_thread_owned(process_t *process,
                                       kernel_thread_t *thread,
                                       bool exit_reaper)
{
    kernel_thread_t **cursor;
    bool release_process_reference = false;
    u64 flags;

    if (!process || !thread || !scheduler_thread_exit_complete(thread))
        return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (thread->process != process ||
        thread->process_state != THREAD_PROCESS_HANDOFF_COMPLETE ||
        process->termination_pending || !thread->process_reference_held ||
        process->thread_list_readers ||
        (process->thread_reap_in_progress && !exit_reaper) ||
        !process->thread_count || !process->threads) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    if (thread->process_initial && process->state != PROCESS_STATE_TERMINATED &&
        !process->exit_cleanup_in_progress) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    cursor = (kernel_thread_t **)&process->threads;
    while (*cursor && *cursor != thread)
        cursor = &(*cursor)->process_next;
    if (*cursor != thread) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    *cursor = thread->process_next;
    if (process->main_thread == thread) process->main_thread = NULL;
    process->thread_count--;
    thread->process = NULL;
    thread->process_next = NULL;
    thread->process_state = THREAD_PROCESS_UNATTACHED;
    thread->process_initial = false;
    __atomic_store_n(&thread->process_exit_requested, false,
                     __ATOMIC_RELAXED);
    thread->process_reference_held = false;
    if (thread->worker_record) {
        process_thread_record_t *record = thread->worker_record;
        if (record->execution == thread) record->execution = NULL;
        thread->worker_record = NULL;
    }
    thread->process_exit_status = 0;
    thread->process_exit_status_valid = false;
    release_process_reference = true;
    spin_unlock_irqrestore(&process_registry_lock, flags);

    if (release_process_reference) object_release(&process->object);
    return true;
}

bool process_detach_thread(process_t *process, struct kernel_thread *thread)
{
    return process_detach_thread_owned(process, thread, false);
}

static bool process_finalize_exit(process_t *process)
{
    u64 flags;
    bool notify = false;

    if (!process || !kernel_stack_release_context_valid()) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state != PROCESS_STATE_TERMINATED ||
        !process->exit_cleanup_complete || process->thread_reap_in_progress ||
        process->thread_list_readers ||
        !process_thread_list_complete_locked(process)) {
        bool done = process->thread_count == 0 &&
            process->state == PROCESS_STATE_TERMINATED &&
            process->exit_cleanup_complete &&
            process->exit_notification_sent;
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return done;
    }
    process->thread_reap_in_progress = true;
    spin_unlock_irqrestore(&process_registry_lock, flags);

    for (;;) {
        kernel_thread_t *thread;
        flags = spin_lock_irqsave(&process_registry_lock);
        thread = process->threads;
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!thread) break;
        if (!process_detach_thread_owned(process, thread, true)) {
            flags = spin_lock_irqsave(&process_registry_lock);
            process->thread_reap_in_progress = false;
            spin_unlock_irqrestore(&process_registry_lock, flags);
            return false;
        }
        /* PID 1 adopts the statically allocated BSP bootstrap scheduler
         * record. Its process ownership ends here, but the scheduler record
         * and linker-backed stack are permanent kernel state. */
        if (!thread->stack_external && !thread_destroy(thread))
            panic("process: completed thread could not be reaped");
    }

    process_worker_records_release_all(process);

    flags = spin_lock_irqsave(&process_registry_lock);
    process->thread_reap_in_progress = false;
    if (!process->exit_notification_sent) {
        process->exit_notification_sent = true;
        notify = true;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (notify) session_process_exited(process);
    return true;
}

process_t *process_current(void)
{
    kernel_thread_t *thread = thread_current();
    return thread ? thread->process : NULL;
}

u64 process_current_pid(void)
{
    process_t *process = process_current();

    return process ? process->pid : 0;
}

u64 process_current_thread_id(void)
{
    kernel_thread_t *thread = thread_current();
    return thread && thread->process ? thread->id : 0;
}

bool process_storage_management_begin(process_t *process)
{
    bool admitted = false;
    u64 flags;

    if (!process) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state == PROCESS_STATE_ACTIVE &&
        !process->termination_pending &&
        !__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE) &&
        !process->storage_management_session &&
        !process->storage_management_authorizing) {
        process->storage_management_authorizing = true;
        admitted = true;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return admitted;
}

bool process_storage_management_finish(process_t *process,
                                       bool pass_authorized)
{
    bool authorized = false;
    u64 flags;

    if (!process) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->storage_management_authorizing) {
        if (pass_authorized && process->state == PROCESS_STATE_ACTIVE &&
            !process->termination_pending &&
            !__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE)) {
            process->storage_management_session = true;
            authorized = true;
        }
        process->storage_management_authorizing = false;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return authorized;
}

int process_storage_authorization_grant(process_t *process, u64 thread_id,
                                        u32 operation)
{
    int result = MG_ERR_PRIVILEGE_REQUIRED;
    u64 flags;

    if (!process || !thread_id || !operation) return MG_ERR_BAD_ARGUMENT;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state == PROCESS_STATE_ACTIVE &&
        !process->termination_pending &&
        !__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE) &&
        process->storage_management_session) {
        if (process->storage_authorized_operation) {
            result = MG_ERR_BUSY;
        } else {
            process->storage_authorized_thread_id = thread_id;
            process->storage_authorized_operation = operation;
            result = MG_OK;
        }
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return result;
}

bool process_storage_authorization_consume(process_t *process, u64 thread_id,
                                           u32 operation)
{
    bool consumed = false;
    u64 flags;

    if (!process || !thread_id || !operation) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state == PROCESS_STATE_ACTIVE &&
        !process->termination_pending &&
        !__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE) &&
        process->storage_management_session &&
        process->storage_authorized_operation == operation &&
        process->storage_authorized_thread_id == thread_id) {
        process->storage_authorized_operation = 0U;
        process->storage_authorized_thread_id = 0U;
        consumed = true;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return consumed;
}

int process_storage_authorization_cancel(process_t *process, u64 thread_id)
{
    int result = MG_ERR_PRIVILEGE_REQUIRED;
    u64 flags;

    if (!process || !thread_id) return MG_ERR_PRIVILEGE_REQUIRED;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state == PROCESS_STATE_ACTIVE &&
        !process->termination_pending &&
        !__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE) &&
        process->storage_management_session) {
        if (!process->storage_authorized_operation) {
            /* Cancellation after a successful or failed attempt is
             * idempotent within the authorized management session. */
            result = MG_OK;
        } else if (process->storage_authorized_thread_id == thread_id) {
            process->storage_authorized_operation = 0U;
            process->storage_authorized_thread_id = 0U;
            result = MG_OK;
        } else {
            result = MG_ERR_ACCESS_DENIED;
        }
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return result;
}

static bool process_path_access(const vfs_node_t *node, void *context)
{
    process_credentials_t credentials;
    return process_get_credentials(context, &credentials) &&
        vfs_check_access_credentials(node, VFS_ACCESS_READ, &credentials);
}

int process_lookup_path_n_access(process_t *process, const char *path,
                                  usize path_length,
                                  bool (*access)(const vfs_node_t *, void *),
                                  void *context, vfs_location_t **out)
{
    if (!out) return VFS_ERR_INVALID_PARAM;
    *out = NULL;
    if (!process || process->state != PROCESS_STATE_ACTIVE || !path ||
        !path_length)
        return VFS_ERR_INVALID_PARAM;
    if (path_length > MG_PATH_MAX_BYTES) return VFS_ERR_NAME_TOO_LONG;
    VFS_LOCATION_AUTO vfs_location_t *cwd = NULL;
    if (path[0] != '/') {
        cwd = process_cwd_acquire(process);
        if (!cwd) return VFS_ERR_DEVICE_GONE;
    }
    return vfs_location_lookup_n(cwd, path, path_length, access, context, out);
}

int process_lookup_path_access(process_t *process, const char *path,
                                bool (*access)(const vfs_node_t *, void *),
                                void *context, vfs_location_t **out)
{
    usize path_length;
    int result = vfs_path_cstr_length(path, &path_length);
    return result == VFS_OK
        ? process_lookup_path_n_access(process, path, path_length,
                                       access, context, out)
        : result;
}

int process_lookup_path_n(process_t *process, const char *path,
                           usize path_length, vfs_location_t **out)
{
    return process_lookup_path_n_access(process, path, path_length,
                                         process_path_access, process, out);
}

int process_lookup_path(process_t *process, const char *path, vfs_location_t **out)
{
    return process_lookup_path_access(process, path, process_path_access, process, out);
}

int process_lookup_parent_n_access(process_t *process, const char *path,
                                    usize path_length,
                                    bool (*access)(const vfs_node_t *, void *),
                                    void *context, vfs_location_t **out,
                                    char *name, usize capacity,
                                    bool *out_trailing_separator)
{
    if (!out) return VFS_ERR_INVALID_PARAM;
    *out = NULL;
    if (out_trailing_separator) *out_trailing_separator = false;
    if (!process || process->state != PROCESS_STATE_ACTIVE || !path ||
        !path_length)
        return VFS_ERR_INVALID_PARAM;
    if (path_length > MG_PATH_MAX_BYTES) return VFS_ERR_NAME_TOO_LONG;
    VFS_LOCATION_AUTO vfs_location_t *cwd = NULL;
    if (path[0] != '/') {
        cwd = process_cwd_acquire(process);
        if (!cwd) return VFS_ERR_DEVICE_GONE;
    }
    return vfs_location_parent_n(cwd, path, path_length, access, context,
                                 out, name, capacity, out_trailing_separator);
}

int process_lookup_parent_access(process_t *process, const char *path,
                                  bool (*access)(const vfs_node_t *, void *),
                                  void *context, vfs_location_t **out,
                                  char *name, usize capacity)
{
    usize path_length;
    int result = vfs_path_cstr_length(path, &path_length);
    return result == VFS_OK
        ? process_lookup_parent_n_access(process, path, path_length, access,
            context, out, name, capacity, NULL)
        : result;
}

int process_lookup_parent_n(process_t *process, const char *path,
                             usize path_length, vfs_location_t **out,
                             char *name, usize capacity,
                             bool *out_trailing_separator)
{
    return process_lookup_parent_n_access(process, path, path_length,
        process_path_access, process, out, name, capacity,
        out_trailing_separator);
}

int process_lookup_parent(process_t *process, const char *path,
                            vfs_location_t **out, char *name, usize capacity)
{
    return process_lookup_parent_access(process, path, process_path_access,
        process, out, name, capacity);
}

int process_cwd_path(process_t *process, char *out, usize capacity, usize *required)
{
    VFS_LOCATION_AUTO vfs_location_t *cwd = process_cwd_acquire(process);
    if (!cwd) return VFS_ERR_DEVICE_GONE;
    return vfs_location_path(cwd, out, capacity, required);
}

int process_chdir_result_n(process_t *process, const char *input,
                            usize input_length)
{
    VFS_LOCATION_AUTO vfs_location_t *location = NULL;
    int result = process_lookup_path_n(process, input, input_length, &location);
    if (result != VFS_OK) return result;
    if (vfs_location_node(location)->type != VFS_TYPE_DIRECTORY)
        return VFS_ERR_NOT_DIRECTORY;
    if (!vfs_location_cwd_retain(location)) return VFS_ERR_DEVICE_GONE;
    u64 flags = spin_lock_irqsave(&process->cwd_lock);
    if (process->state != PROCESS_STATE_ACTIVE) {
        spin_unlock_irqrestore(&process->cwd_lock, flags);
        vfs_location_cwd_release(location);
        return VFS_ERR_CANCELLED;
    }
    vfs_location_t *old = process->cwd_location;
    process->cwd_location = location;
    spin_unlock_irqrestore(&process->cwd_lock, flags);
    vfs_location_cwd_release(old);
    return VFS_OK;
}

int process_chdir_result(process_t *process, const char *input)
{
    usize input_length;
    int result = vfs_path_cstr_length(input, &input_length);
    return result == VFS_OK
        ? process_chdir_result_n(process, input, input_length) : result;
}

bool process_chdir(process_t *process, const char *input)
{
    return process_chdir_result(process, input) == VFS_OK;
}


static bool process_memory_size_to_pages(usize size, usize *out_pages)
{
    if (!size || !out_pages || size > ~(usize)0 - (VMM_PAGE_SIZE - 1)) {
        return false;
    }
    *out_pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    return *out_pages != 0;
}

static uintptr_t process_memory_find_space(process_t *process,
                                           usize page_count)
{
    process_memory_mapping_t *mapping;
    process_user_region_t *region;
    uintptr_t candidate = VMM_USER_ANON_BASE;
    usize request_bytes;

    if (!process || page_count >
        (VMM_USER_ANON_LIMIT - VMM_USER_ANON_BASE) / VMM_PAGE_SIZE) {
        return 0;
    }
    request_bytes = page_count * VMM_PAGE_SIZE;
    for (;;) {
        uintptr_t candidate_end;
        uintptr_t skip_to = candidate;
        bool conflict = false;

        if (candidate > VMM_USER_ANON_LIMIT ||
            request_bytes > VMM_USER_ANON_LIMIT - candidate)
            return 0;
        candidate_end = candidate + request_bytes;
        mapping = (process_memory_mapping_t *)process->memory_mappings;
        while (mapping) {
            usize bytes;
            uintptr_t end;
            if (mapping->page_count > ~(usize)0 / VMM_PAGE_SIZE)
                return 0;
            bytes = mapping->page_count * VMM_PAGE_SIZE;
            if (mapping->address > ~(uintptr_t)0 - bytes) return 0;
            end = mapping->address + bytes;
            if (end > VMM_USER_ANON_BASE &&
                mapping->address < VMM_USER_ANON_LIMIT &&
                candidate < end && mapping->address < candidate_end) {
                conflict = true;
                if (end > skip_to) skip_to = end;
            }
            mapping = mapping->next;
        }
        region = process->user_regions;
        while (region) {
            usize bytes;
            uintptr_t end;
            if (region->reservation_pages > ~(usize)0 / VMM_PAGE_SIZE)
                return 0;
            bytes = region->reservation_pages * VMM_PAGE_SIZE;
            if (region->reservation_base > ~(uintptr_t)0 - bytes) return 0;
            end = region->reservation_base + bytes;
            if (end > VMM_USER_ANON_BASE &&
                region->reservation_base < VMM_USER_ANON_LIMIT &&
                candidate < end && region->reservation_base < candidate_end) {
                conflict = true;
                if (end > skip_to) skip_to = end;
            }
            region = region->next;
        }
        if (conflict) {
            candidate = skip_to;
            continue;
        }
        if (vmm_user_range_unmapped(process->address_space, candidate,
                                    request_bytes))
            return candidate;
        if (candidate > VMM_USER_ANON_LIMIT - VMM_PAGE_SIZE) return 0;
        candidate += VMM_PAGE_SIZE;
    }
}

static void process_user_region_insert_locked(process_t *process,
                                             process_user_region_t *region)
{
    process_user_region_t **cursor = &process->user_regions;
    while (*cursor && (*cursor)->reservation_base < region->reservation_base)
        cursor = &(*cursor)->next;
    region->next = *cursor;
    *cursor = region;
}

bool process_user_register_initial_stack(process_t *process,
                                         uintptr_t stack_top,
                                         usize stack_pages)
{
    process_user_region_t *region;
    uintptr_t mapping_base;
    uintptr_t reservation_base;
    usize mapping_bytes;

    if (!process || !process->address_space || !stack_pages ||
        stack_pages > ~(usize)0 / VMM_PAGE_SIZE ||
        stack_top & (VMM_PAGE_SIZE - 1U))
        return false;
    mapping_bytes = stack_pages * VMM_PAGE_SIZE;
    if (stack_top < mapping_bytes + VMM_PAGE_SIZE) return false;
    mapping_base = stack_top - mapping_bytes;
    reservation_base = mapping_base - VMM_PAGE_SIZE;
    region = kmalloc(sizeof(*region));
    if (!region) return false;
    *region = (process_user_region_t){
        .reservation_base = reservation_base,
        .reservation_pages = stack_pages + 1U,
        .mapping_base = mapping_base,
        .mapping_pages = stack_pages,
        .kind = PROCESS_USER_REGION_INITIAL_STACK,
        .next = NULL,
    };
    if (!vmm_user_range_unmapped(process->address_space, reservation_base,
                                 VMM_PAGE_SIZE)) {
        kfree(region);
        return false;
    }
    if (!mutex_lock(&process->memory_lock)) {
        kfree(region);
        return false;
    }
    /* The initial stack is installed before launch publication. Reject an
     * accidental overlap with any process mapping or prior reservation. */
    for (process_user_region_t *other = process->user_regions; other;
         other = other->next) {
        uintptr_t end = other->reservation_base +
                        other->reservation_pages * VMM_PAGE_SIZE;
        if (reservation_base < end && other->reservation_base < stack_top) {
            mutex_unlock(&process->memory_lock);
            kfree(region);
            return false;
        }
    }
    for (process_memory_mapping_t *other = process->memory_mappings; other;
         other = other->next) {
        uintptr_t end = other->address + other->page_count * VMM_PAGE_SIZE;
        if (reservation_base < end && other->address < stack_top) {
            mutex_unlock(&process->memory_lock);
            kfree(region);
            return false;
        }
    }
    process_user_region_insert_locked(process, region);
    mutex_unlock(&process->memory_lock);
    return true;
}

bool process_user_tls_template_adopt(process_t *process, u8 *image,
                                     usize file_size, usize memory_size,
                                     usize alignment)
{
    if (!process || (!image && file_size) || process->tls_present ||
        file_size > memory_size || !memory_size || !alignment ||
        (alignment & (alignment - 1U)))
        return false;
    process->tls_image = image;
    process->tls_file_size = file_size;
    process->tls_memory_size = memory_size;
    process->tls_alignment = alignment < PROCESS_TLS_MIN_ALIGNMENT
        ? PROCESS_TLS_MIN_ALIGNMENT : alignment;
    process->tls_present = true;
    return true;
}

static bool process_memory_release_mapping(process_t *process,
                                           process_memory_mapping_t *mapping)
{
    bool released = true;
    usize i;

    if (!process || !mapping) return false;
    for (i = 0; i < mapping->page_count; i++) {
        phys_addr_t frame;
        uintptr_t address = mapping->address + i * VMM_PAGE_SIZE;
        if (!vmm_unmap_user_page(process->address_space,
                                 (void *)address, &frame)) {
            released = false;
            continue;
        }
        pmm_free_frame(frame);
    }
    return released;
}

static bool process_user_region_copy_to_frames(const phys_addr_t *frames,
                                              usize page_count,
                                              uintptr_t mapping_base,
                                              uintptr_t destination,
                                              const void *source,
                                              usize length)
{
    const u8 *bytes = source;

    if ((!source && length) || !frames || !page_count) return false;
    while (length) {
        usize index = (usize)((destination - mapping_base) / VMM_PAGE_SIZE);
        usize in_page = (usize)(destination & (VMM_PAGE_SIZE - 1U));
        usize amount = VMM_PAGE_SIZE - in_page;
        u8 *target;
        if (index >= page_count) return false;
        if (amount > length) amount = length;
        if (!vmm_direct_map_valid(frames[index])) return false;
        target = (u8 *)phys_to_virt(frames[index]);
        if (!target) return false;
        memcpy(target + in_page, bytes, amount);
        bytes += amount;
        destination += amount;
        length -= amount;
    }
    return true;
}

static process_user_region_t *process_user_region_create(
    process_t *process, process_user_region_kind_t kind,
    usize guard_pages, usize mapping_pages, usize alignment,
    const void *initial_data, usize initial_size, usize initial_offset,
    usize tls_block_size, uintptr_t *out_data_start)
{
    process_user_region_t *region = NULL;
    phys_addr_t *frames = NULL;
    uintptr_t reservation_base;
    uintptr_t mapping_base;
    uintptr_t data_start = 0;
    usize reservation_pages;
    usize map_bytes;
    usize reserve_bytes;
    usize i;
    usize mapped = 0;

    if (out_data_start) *out_data_start = 0;
    if (!process || !process->address_space || !mapping_pages ||
        guard_pages > ~(usize)0 - mapping_pages ||
        mapping_pages > ~(usize)0 / VMM_PAGE_SIZE ||
        guard_pages + mapping_pages >
            (VMM_USER_ANON_LIMIT - VMM_USER_ANON_BASE) / VMM_PAGE_SIZE ||
        !alignment || (alignment & (alignment - 1U)) ||
        (tls_block_size && initial_size > tls_block_size) ||
        mapping_pages > ~(usize)0 / sizeof(*frames))
        return NULL;
    reservation_pages = guard_pages + mapping_pages;
    reserve_bytes = reservation_pages * VMM_PAGE_SIZE;
    map_bytes = mapping_pages * VMM_PAGE_SIZE;

    region = kmalloc(sizeof(*region));
    frames = kmalloc(mapping_pages * sizeof(*frames));
    if (!region || !frames) goto fail;
    memset(frames, 0, mapping_pages * sizeof(*frames));
    for (i = 0; i < mapping_pages; i++) {
        frames[i] = pmm_alloc_frame();
        if (!frames[i]) goto fail_frames;
        if (!vmm_direct_map_valid(frames[i])) goto fail_frames;
        memset(phys_to_virt(frames[i]), 0, VMM_PAGE_SIZE);
    }

    if (!mutex_lock(&process->memory_lock)) goto fail_frames;
    reservation_base = process_memory_find_space(process, reservation_pages);
    if (!reservation_base || reserve_bytes >
            VMM_USER_ANON_LIMIT - reservation_base) {
        mutex_unlock(&process->memory_lock);
        goto fail_frames;
    }
    mapping_base = reservation_base + guard_pages * VMM_PAGE_SIZE;
    if (guard_pages && !vmm_user_range_unmapped(
            process->address_space, reservation_base,
            guard_pages * VMM_PAGE_SIZE)) {
        mutex_unlock(&process->memory_lock);
        goto fail_frames;
    }
    if (tls_block_size) {
        uintptr_t thread_pointer;
        uintptr_t dtv = 0;
        bool aligned;
        data_start = process_address_align_up(mapping_base, alignment,
                                              &aligned);
        if (!aligned || data_start < mapping_base ||
            data_start - mapping_base > map_bytes ||
            tls_block_size > map_bytes - (data_start - mapping_base) ||
            (initial_size && !process_user_region_copy_to_frames(
                frames, mapping_pages, mapping_base, data_start, initial_data,
                initial_size))) {
            mutex_unlock(&process->memory_lock);
            goto fail_frames;
        }
        if (kind == PROCESS_USER_REGION_THREAD_TLS) {
            if (data_start > ~(uintptr_t)0 - tls_block_size) {
                mutex_unlock(&process->memory_lock);
                goto fail_frames;
            }
            thread_pointer = data_start + tls_block_size;
            if (thread_pointer < mapping_base ||
                thread_pointer - mapping_base > map_bytes ||
                PROCESS_TLS_TCB_BYTES > map_bytes -
                    (thread_pointer - mapping_base) ||
                !process_user_region_copy_to_frames(frames, mapping_pages,
                    mapping_base, thread_pointer, &thread_pointer,
                    sizeof(thread_pointer)) ||
                !process_user_region_copy_to_frames(frames, mapping_pages,
                    mapping_base, thread_pointer + sizeof(thread_pointer),
                    &dtv, sizeof(dtv))) {
                mutex_unlock(&process->memory_lock);
                goto fail_frames;
            }
        }
    } else {
        if (initial_offset > map_bytes ||
            initial_size > map_bytes - initial_offset) {
            mutex_unlock(&process->memory_lock);
            goto fail_frames;
        }
        data_start = mapping_base + initial_offset;
        if (initial_size && !process_user_region_copy_to_frames(
                frames, mapping_pages, mapping_base, data_start, initial_data,
                initial_size)) {
            mutex_unlock(&process->memory_lock);
            goto fail_frames;
        }
    }

    *region = (process_user_region_t){
        .reservation_base = reservation_base,
        .reservation_pages = reservation_pages,
        .mapping_base = mapping_base,
        .mapping_pages = mapping_pages,
        .kind = kind,
        .next = NULL,
    };
    for (mapped = 0; mapped < mapping_pages; mapped++) {
        if (!vmm_map_user_page(process->address_space,
                (void *)(mapping_base + mapped * VMM_PAGE_SIZE), frames[mapped],
                PTE_USER | PTE_READWRITE | PTE_NX))
            break;
    }
    if (mapped != mapping_pages) {
        bool deferred = false;
        if (mapped) {
            vmm_user_range_revoke_result_t revoke_result =
                vmm_user_range_revoke(process->address_space,
                    (void *)mapping_base, mapped);
            if (revoke_result == VMM_USER_RANGE_REVOKE_INVALID)
                panic("process: partial user-region revoke failed");
            if (revoke_result == VMM_USER_RANGE_REVOKE_RETRY ||
                !vmm_unmap_user_range(process->address_space,
                    (void *)mapping_base, mapped)) {
                /* A sibling may already hold a syscall lease on a page
                 * exposed by the shared address space, or the serialized VMM
                 * operation lock may be temporarily unavailable. Keep the
                 * frame ownership and reservation until revocation and unmap
                 * can both complete. */
                region->mapping_pages = mapped;
                region->release_pending = true;
                region->revoked =
                    revoke_result == VMM_USER_RANGE_REVOKED;
                process_user_region_insert_locked(process, region);
                __atomic_store_n(&process->user_region_reap_pending, true,
                                 __ATOMIC_RELEASE);
                deferred = true;
            }
        }
        for (i = mapped; i < mapping_pages; i++) pmm_free_frame(frames[i]);
        mutex_unlock(&process->memory_lock);
        kfree(frames);
        if (!deferred) kfree(region);
        else process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);
        return NULL;
    }
    process_user_region_insert_locked(process, region);
    mutex_unlock(&process->memory_lock);
    kfree(frames);
    if (out_data_start) *out_data_start = data_start;
    return region;

fail_frames:
    if (frames) {
        for (i = 0; i < mapping_pages; i++)
            if (frames[i]) pmm_free_frame(frames[i]);
    }
fail:
    if (frames) kfree(frames);
    if (region) kfree(region);
    return NULL;
}

/* Thread handles are reserved before worker ownership is published.  These
 * helpers run while process_registry_lock is held, so exit admission, record
 * ownership, handle visibility, and run-queue publication form one commit.
 * The handle lock also serializes against table growth; no growth mutex is
 * needed because these operations never sleep. */
static bool process_thread_handle_activate_locked(process_t *process,
                                                   process_handle_t handle)
{
    u32 index;
    u16 generation;
    u64 flags;
    process_handle_slot_t *slot;

    if (!process || !decode_handle(handle, &index, &generation)) return false;
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!process->handle_table || index >= process->handle_capacity) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        return false;
    }
    slot = &handle_slots(process)[index];
    if (process->state != PROCESS_STATE_ACTIVE || !slot->reserved ||
        slot->active || slot->generation != generation || !slot->object) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        return false;
    }
    slot->active = true;
    slot->reserved = false;
    process_handle_revision_advance(process);
    spin_unlock_irqrestore(&process->handle_lock, flags);
    return true;
}

static bool process_thread_handle_deactivate_locked(process_t *process,
                                                     process_handle_t handle)
{
    u32 index;
    u16 generation;
    u64 flags;
    process_handle_slot_t *slot;

    if (!process || !decode_handle(handle, &index, &generation)) return false;
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!process->handle_table || index >= process->handle_capacity) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        return false;
    }
    slot = &handle_slots(process)[index];
    if (!slot->active || slot->reserved || slot->generation != generation ||
        !slot->object) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        return false;
    }
    slot->active = false;
    slot->reserved = true;
    process_handle_revision_advance(process);
    spin_unlock_irqrestore(&process->handle_lock, flags);
    return true;
}

static process_region_release_result_t process_user_region_release(
    process_t *process, process_user_region_t *region)
{
    process_user_region_t **cursor;
    if (!process || !region || !mutex_lock(&process->memory_lock))
        return PROCESS_REGION_RELEASE_INVALID;
    cursor = &process->user_regions;
    while (*cursor && *cursor != region) cursor = &(*cursor)->next;
    if (*cursor != region) {
        mutex_unlock(&process->memory_lock);
        return PROCESS_REGION_RELEASE_INVALID;
    }
    if (!region->revoked) {
        vmm_user_range_revoke_result_t revoke_result =
            vmm_user_range_revoke(process->address_space,
                (void *)region->mapping_base, region->mapping_pages);
        if (revoke_result == VMM_USER_RANGE_REVOKE_RETRY) {
            region->release_pending = true;
            __atomic_store_n(&process->user_region_reap_pending, true,
                             __ATOMIC_RELEASE);
            mutex_unlock(&process->memory_lock);
            return PROCESS_REGION_RELEASE_DEFERRED;
        }
        if (revoke_result != VMM_USER_RANGE_REVOKED) {
            mutex_unlock(&process->memory_lock);
            return PROCESS_REGION_RELEASE_INVALID;
        }
        region->revoked = true;
    }
    region->release_pending = true;
    if (!vmm_unmap_user_range(process->address_space,
            (void *)region->mapping_base, region->mapping_pages)) {
        __atomic_store_n(&process->user_region_reap_pending, true,
                         __ATOMIC_RELEASE);
        mutex_unlock(&process->memory_lock);
        return PROCESS_REGION_RELEASE_DEFERRED;
    }
    *cursor = region->next;
    region->next = NULL;
    bool pending = false;
    for (process_user_region_t *item = process->user_regions; item;
         item = item->next) {
        if (item->release_pending) {
            pending = true;
            break;
        }
    }
    __atomic_store_n(&process->user_region_reap_pending, pending,
                     __ATOMIC_RELEASE);
    mutex_unlock(&process->memory_lock);
    kfree(region);
    return PROCESS_REGION_RELEASED;
}

static bool process_user_region_set_worker_owned(
    process_t *process, process_user_region_t *stack_region,
    process_user_region_t *tls_region, bool worker_owned)
{
    bool stack_found = false;
    bool tls_found = tls_region == NULL;

    if (!process || !stack_region ||
        !mutex_lock(&process->memory_lock))
        return false;
    for (process_user_region_t *region = process->user_regions; region;
         region = region->next) {
        if (region == stack_region) stack_found = true;
        if (region == tls_region) tls_found = true;
    }
    if (stack_found && tls_found) {
        stack_region->worker_owned = worker_owned;
        if (tls_region) tls_region->worker_owned = worker_owned;
    }
    mutex_unlock(&process->memory_lock);
    return stack_found && tls_found;
}

static void process_user_regions_reap_pending(process_t *process)
{
    process_user_region_t *released = NULL;
    process_user_region_t **cursor;
    bool pending = false;

    if (!process || !__atomic_load_n(&process->user_region_reap_pending,
                                     __ATOMIC_ACQUIRE) ||
        !mutex_lock(&process->memory_lock))
        return;
    cursor = &process->user_regions;
    while (*cursor) {
        process_user_region_t *region = *cursor;
        if (!region->release_pending) {
            cursor = &region->next;
            continue;
        }
        if (region->worker_owned) {
            pending = true;
            cursor = &region->next;
            continue;
        }
        if (!region->revoked) {
            vmm_user_range_revoke_result_t revoke_result =
                vmm_user_range_revoke(process->address_space,
                    (void *)region->mapping_base, region->mapping_pages);
            if (revoke_result == VMM_USER_RANGE_REVOKE_INVALID)
                panic("process: deferred user-region mapping ownership lost");
            if (revoke_result == VMM_USER_RANGE_REVOKE_RETRY) {
                pending = true;
                cursor = &region->next;
                continue;
            }
            region->revoked = true;
        }
        if (!vmm_unmap_user_range(process->address_space,
                (void *)region->mapping_base, region->mapping_pages)) {
            pending = true;
            cursor = &region->next;
            continue;
        }
        *cursor = region->next;
        region->next = released;
        released = region;
    }
    __atomic_store_n(&process->user_region_reap_pending, pending,
                     __ATOMIC_RELEASE);
    mutex_unlock(&process->memory_lock);
    while (released) {
        process_user_region_t *next = released->next;
        kfree(released);
        released = next;
    }
}

/* Caller holds process_registry_lock. This flag avoids a process-wide record
 * scan on every ordinary syscall while still allowing deferred VMM lease
 * cleanup to be retried by the syscall that releases the final lease. */
static bool process_worker_reap_needed_locked(process_t *process)
{
    if (!process || process->state != PROCESS_STATE_ACTIVE) return false;
    for (process_thread_record_t *record = process->thread_records; record;
         record = record->next) {
        if (record->completed &&
            (record->reclaiming || !record->kernel_stack_released ||
             record->stack_region || record->tls_region ||
             record->execution ||
             (record->listed && (record->joined || record->detached))))
            return true;
    }
    return false;
}

/* A completed worker can have released all execution resources while a
 * process-thread-list reader temporarily prevents unlinking its scheduler
 * object. Retry immediately after the last reader leaves; do not leave the
 * terminated execution record waiting for an unrelated future syscall. */
static bool process_worker_reap_immediate_locked(process_t *process)
{
    if (!process || process->state != PROCESS_STATE_ACTIVE ||
        process->termination_pending || process->thread_list_readers ||
        process->thread_reap_in_progress)
        return false;
    for (process_thread_record_t *record = process->thread_records; record;
         record = record->next) {
        if (record->listed && record->completed && !record->reclaiming &&
            record->execution && record->kernel_stack_released &&
            !record->stack_region && !record->tls_region)
            return true;
    }
    return false;
}

/* A worker reap can unmap user TLS and stacks that were active on another
 * CPU. Unlike a local-only stack release, that VMM operation requires local
 * interrupts enabled so remote CPUs can acknowledge the TLB shootdown. Do
 * not infer this from the current online count: an address space may still
 * have remote active-CPU targets during a CPU transition. */
static bool process_worker_reclaim_context_valid(void)
{
    kernel_thread_t *current = thread_current();
    u64 flags = process_cli_save();
    process_cli_restore(flags);
    return current && !current->waiting_mutex && (flags & (1ULL << 9)) != 0 &&
           kernel_stack_release_context_valid();
}

void process_worker_reclaim_pending(process_t *process)
{
    process_thread_record_t *record;
    u64 flags;
    bool retry_available = true;

    if (!process ||
        (!__atomic_load_n(&process->worker_reap_pending, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&process->user_region_reap_pending,
                          __ATOMIC_ACQUIRE)) ||
        !process_worker_reclaim_context_valid())
        return;

    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state != PROCESS_STATE_ACTIVE || process->termination_pending ||
        process->worker_reapers ||
        process->thread_reap_in_progress || !object_reference(&process->object)) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return;
    }
    process->worker_reapers = 1U;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    process_user_regions_reap_pending(process);

    for (;;) {
      flags = spin_lock_irqsave(&process_registry_lock);
      record = process->thread_records;
      if (record && !object_reference(&record->object))
          panic("process: worker record reference overflow");
      spin_unlock_irqrestore(&process_registry_lock, flags);

      while (record) {
        process_thread_record_t *next;
        kernel_thread_t *execution = NULL;
        process_user_region_t *stack_region = NULL;
        process_user_region_t *tls_region = NULL;
        bool claimed = false;
        bool stack_released = true;
        bool tls_released = true;
        bool execution_stack_released = true;
        bool drop_list_reference = false;
        process_region_release_result_t stack_release_result =
            PROCESS_REGION_RELEASED;
        process_region_release_result_t tls_release_result =
            PROCESS_REGION_RELEASED;

        flags = spin_lock_irqsave(&process_registry_lock);
        next = record->next;
        if (next && !object_reference(&next->object))
            panic("process: next worker record reference overflow");
        if (process->state == PROCESS_STATE_ACTIVE &&
            !process->termination_pending &&
            !process->thread_reap_in_progress &&
            record->listed && record->completed && !record->reclaiming &&
            object_reference(&record->object)) {
            record->reclaiming = true;
            execution = record->execution;
            stack_region = record->stack_region;
            tls_region = record->tls_region;
            claimed = true;
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);

        if (claimed) {
            if (execution)
                execution_stack_released =
                    thread_release_terminated_stack(execution);
            if (tls_region)
                tls_release_result = process_user_region_release(
                    process, tls_region);
            if (stack_region)
                stack_release_result = process_user_region_release(
                    process, stack_region);
            if (tls_release_result == PROCESS_REGION_RELEASE_INVALID)
                panic("process: worker TLS region ownership lost during reap");
            if (stack_release_result == PROCESS_REGION_RELEASE_INVALID)
                panic("process: worker stack region ownership lost during reap");
            tls_released = tls_release_result == PROCESS_REGION_RELEASED;
            stack_released = stack_release_result ==
                PROCESS_REGION_RELEASED;

            /* User mappings can remain deferred while a sibling's VMM lease
             * pins them; the completion record retains those region nodes
             * until a later lease-release retry. */
            bool retire_execution = false;
            flags = spin_lock_irqsave(&process_registry_lock);
            if (execution_stack_released && execution)
                record->kernel_stack_released = true;
            if (tls_released && record->tls_region == tls_region) {
                record->tls_region = NULL;
                if (execution && execution->user_context.tls_region ==
                                     tls_region)
                    execution->user_context.tls_region = NULL;
            }
            if (stack_released && record->stack_region == stack_region) {
                record->stack_region = NULL;
                if (execution && execution->user_context.stack_region ==
                                     stack_region)
                    execution->user_context.stack_region = NULL;
            }
            if (execution_stack_released && execution &&
                process->state == PROCESS_STATE_ACTIVE &&
                !process->termination_pending)
                retire_execution = true;
            spin_unlock_irqrestore(&process_registry_lock, flags);

            /* The completion record carries join authority and result. The
             * scheduler object is no longer needed once its final stack has
             * been released, even while the worker remains joinable. */
            if (retire_execution &&
                process_detach_thread(process, execution)) {
                if (!thread_destroy(execution))
                    panic("process: completed worker could not be reaped");
                execution = NULL;
            }

            flags = spin_lock_irqsave(&process_registry_lock);
            if (execution && !record->execution)
                panic("process: worker execution owner disappeared");
            /* The record remains joinable after its scheduler object is
             * gone. Remove it only when join/detach has consumed that right
             * and every lease-deferred user region has been released. */
            if (!record->execution && record->listed &&
                !record->stack_region && !record->tls_region &&
                (record->joined || record->detached)) {
                process_thread_record_t **cursor = &process->thread_records;
                while (*cursor && *cursor != record)
                    cursor = &(*cursor)->next;
                if (*cursor == record) {
                    *cursor = record->next;
                    record->next = NULL;
                    record->listed = false;
                    record->process = NULL;
                    drop_list_reference = true;
                }
            }
            record->reclaiming = false;
            (void)process_publish_terminated_locked(process);
            spin_unlock_irqrestore(&process_registry_lock, flags);

            if (drop_list_reference) object_release(&record->object);
            object_release(&record->object); /* claimed reaper pin */
            object_release(&record->object); /* traversal/reaper pin */
        } else {
            object_release(&record->object); /* traversal pin */
        }
        record = next;
      }
      flags = spin_lock_irqsave(&process_registry_lock);
      bool retry_immediate =
          retry_available &&
          process_worker_reap_immediate_locked(process);
      if (retry_immediate) retry_available = false;
      __atomic_store_n(&process->worker_reap_pending,
                       process_worker_reap_needed_locked(process) ||
                           retry_immediate,
                       __ATOMIC_RELEASE);
      spin_unlock_irqrestore(&process_registry_lock, flags);
      if (!retry_immediate) break;
    }
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->worker_reapers != 1U)
        panic("process: cleanup operation ownership lost");
    process->worker_reapers = 0U;
    bool finish_exit = process->state == PROCESS_STATE_EXITING;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (finish_exit) (void)process_cleanup_exit_resources(process);
    object_release(&process->object);
}

static process_user_region_t *process_user_region_find(
    process_t *process, process_user_region_kind_t kind)
{
    process_user_region_t *found = NULL;
    if (!process || !mutex_lock(&process->memory_lock)) return NULL;
    for (process_user_region_t *region = process->user_regions; region;
         region = region->next) {
        if (region->kind == kind) {
            found = region;
            break;
        }
    }
    mutex_unlock(&process->memory_lock);
    return found;
}

static void process_user_region_metadata_release(process_t *process)
{
    process_user_region_t *region;
    if (!process) return;
    region = process->user_regions;
    process->user_regions = NULL;
    while (region) {
        process_user_region_t *next = region->next;
        kfree(region);
        region = next;
    }
    if (process->tls_image) kfree(process->tls_image);
    process->tls_image = NULL;
}

static void process_memory_release_all(process_t *process)
{
    process_memory_mapping_t *mapping;

    if (!process) return;
    if (!mutex_lock(&process->memory_lock))
        panic("process: failed to lock address-space mappings");
    mapping = (process_memory_mapping_t *)process->memory_mappings;
    process->memory_mappings = NULL;
    mutex_unlock(&process->memory_lock);
    while (mapping) {
        process_memory_mapping_t *next = mapping->next;
        (void)process_memory_release_mapping(process, mapping);
        kfree(mapping);
        mapping = next;
    }
}

i64 process_memory_map(process_t *process, usize size,
                       uintptr_t *out_address)
{
    process_memory_mapping_t *mapping;
    process_memory_mapping_t **cursor;
    uintptr_t address;
    phys_addr_t *frames;
    usize page_count;
    usize i;

    if (!process || process->state != PROCESS_STATE_ACTIVE || !out_address ||
        !process_memory_size_to_pages(size, &page_count)) {
        return MG_ERR_BAD_ARGUMENT;
    }
    mapping = (process_memory_mapping_t *)kmalloc(sizeof(*mapping));
    if (!mapping) return MG_ERR_NO_MEMORY;
    if (page_count > ~(usize)0 / sizeof(*frames)) {
        kfree(mapping);
        return MG_ERR_BAD_ARGUMENT;
    }
    frames = (phys_addr_t *)kmalloc(page_count * sizeof(*frames));
    if (!frames) {
        kfree(mapping);
        return MG_ERR_NO_MEMORY;
    }
    mapping->page_count = page_count;
    mapping->next = NULL;

    /* Acquire physical pages before taking the mapping-list lock.  The VMM
     * user mapper may allocate page-table ownership metadata through the
     * heap, so keeping PMM acquisition outside this lock avoids a PMM -> VMM
     * metadata inversion. */
    for (i = 0; i < page_count; i++) {
        frames[i] = pmm_alloc_frame();
        if (!frames[i]) {
            while (i) pmm_free_frame(frames[--i]);
            kfree(frames);
            kfree(mapping);
            return MG_ERR_NO_MEMORY;
        }
    }

    if (!mutex_lock(&process->memory_lock)) {
        for (i = 0; i < page_count; i++)
            pmm_free_frame(frames[i]);
        kfree(frames);
        kfree(mapping);
        return MG_ERR_WOULD_BLOCK;
    }
    address = process_memory_find_space(process, page_count);
    if (!address) {
        mutex_unlock(&process->memory_lock);
        for (i = 0; i < page_count; i++) pmm_free_frame(frames[i]);
        kfree(frames);
        kfree(mapping);
        return MG_ERR_NO_MEMORY;
    }
    mapping->address = address;
    for (i = 0; i < page_count; i++) {
        if (!vmm_map_user_page(process->address_space,
                                         (void *)(address + i * VMM_PAGE_SIZE),
                                         frames[i],
                                         PTE_USER | PTE_READWRITE | PTE_NX)) {
            for (usize mapped = 0; mapped < i; mapped++) {
                phys_addr_t mapped_frame;
                if (vmm_unmap_user_page(
                        process->address_space,
                        (void *)(address + mapped * VMM_PAGE_SIZE),
                                        &mapped_frame)) {
                    pmm_free_frame(mapped_frame);
                } else {
                    pmm_free_frame(frames[mapped]);
                }
            }
            for (usize remaining = i; remaining < page_count; remaining++)
                pmm_free_frame(frames[remaining]);
            mutex_unlock(&process->memory_lock);
            kfree(frames);
            kfree(mapping);
            return MG_ERR_NO_MEMORY;
        }
    }

    cursor = (process_memory_mapping_t **)&process->memory_mappings;
    while (*cursor && (*cursor)->address < address) cursor = &(*cursor)->next;
    mapping->next = *cursor;
    *cursor = mapping;
    *out_address = address;
    mutex_unlock(&process->memory_lock);
    kfree(frames);
    return MG_OK;
}

i64 process_memory_unmap(process_t *process, uintptr_t address)
{
    process_memory_mapping_t **cursor;
    process_memory_mapping_t *mapping;

    if (!process || process->state != PROCESS_STATE_ACTIVE ||
        (address & (VMM_PAGE_SIZE - 1))) {
        return MG_ERR_BAD_ARGUMENT;
    }
    if (!mutex_lock(&process->memory_lock))
        return MG_ERR_WOULD_BLOCK;
    cursor = (process_memory_mapping_t **)&process->memory_mappings;
    while (*cursor && (*cursor)->address != address) cursor = &(*cursor)->next;
    if (!*cursor) {
        mutex_unlock(&process->memory_lock);
        return MG_ERR_NOT_FOUND;
    }
    mapping = *cursor;
    if (!vmm_unmap_user_range(process->address_space,
                              (void *)mapping->address,
                              mapping->page_count)) {
        mutex_unlock(&process->memory_lock);
        return MG_ERR_BUSY;
    }
    *cursor = mapping->next;
    mutex_unlock(&process->memory_lock);
    kfree(mapping);
    return MG_OK;
}

static bool process_cleanup_exit_resources(process_t *process)
{
    u64 flags;

    if (!process) return false;
    if (!process_cleanup_context_valid()) {
        /* Teardown takes mutexes and can wait for remote TLB acknowledgments.
         * IF alone is insufficient while a resumed mutex waiter still owns
         * an unconsumed handoff marker. Retry on the dedicated worker. */
        process_cleanup_worker_wake(process);
        return false;
    }
    flags = spin_lock_irqsave(&process_registry_lock);
    if (!process->launch_ready ||
        process->state != PROCESS_STATE_EXITING || process->active_syscalls ||
        !process_thread_list_complete_locked(process) ||
        process->exit_cleanup_complete || process->exit_cleanup_in_progress) {
        bool complete = process->exit_cleanup_complete;
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return complete;
    }
    process->exit_cleanup_in_progress = true;
    spin_unlock_irqrestore(&process_registry_lock, flags);

    /* This path runs only after every process thread has completed its
     * scheduler/CR3 handoff and every admitted syscall lease has been
     * released. The caller is on another live scheduler stack. */
    terminal_output_finish_for_process(process->pid,
                                       &process->terminal_output);
    (void)terminal_alternate_leave_process(process->pid);
    terminal_force_end_batch();
    ipc_process_exit(process);
    process_memory_release_all(process);
    process_handle_close_all(process);
    process_cwd_clear(process);

    flags = spin_lock_irqsave(&process_registry_lock);
    process->exit_cleanup_complete = true;
    process->exit_cleanup_in_progress = false;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    (void)process_finish_termination(process);
    return true;
}

static bool process_cleanup_context_valid(void)
{
    u64 current_flags;
    kernel_thread_t *current = thread_current();

    __asm__ volatile("pushfq; popq %0" : "=r"(current_flags) :: "memory");
    return current && !current->waiting_mutex && !spinlock_depth_current() &&
        (current_flags & (1ULL << 9)) &&
        kernel_stack_release_context_valid();
}

static bool process_finish_termination(process_t *process)
{
    bool terminated;
    u64 flags;

    if (!process) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    (void)process_publish_terminated_locked(process);
    terminated = process->state == PROCESS_STATE_TERMINATED;
    spin_unlock_irqrestore(&process_registry_lock, flags);

    return terminated;
}

bool process_exit(process_t *process, i32 status)
{
    kernel_thread_t *current = thread_current();
    bool first_request = false;
    u64 flags;

    if (!process) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state == PROCESS_STATE_TERMINATED ||
        (process->state == PROCESS_STATE_ACTIVE &&
         process->termination_pending &&
         !process->external_termination_in_progress)) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    if (process->state == PROCESS_STATE_ACTIVE) {
        process->state = PROCESS_STATE_EXITING;
        process->termination_pending = true;
        process_storage_authorization_clear_locked(process);
        /* A self-exit may win against an external termination reservation.
         * The scheduler's group preflight cannot concurrently commit while
         * this process thread is running, so the external attempt will fail
         * atomically and release its reservation. */
        process->external_termination_in_progress = false;
        __atomic_store_n(&process->exit_requested, true, __ATOMIC_RELEASE);
        process->exit_status = status;
        first_request = true;
        for (kernel_thread_t *thread = process->threads; thread;
             thread = thread->process_next) {
            __atomic_store_n(&thread->process_exit_requested, true,
                             __ATOMIC_RELEASE);
            if (thread->process_state == THREAD_PROCESS_ATTACHED)
                thread->process_state = THREAD_PROCESS_EXIT_REQUESTED;
        }
    } else if (process->state != PROCESS_STATE_EXITING) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    if (current && current->process == process &&
        current->process_state != THREAD_PROCESS_HANDOFF_COMPLETE) {
        current->process_state = THREAD_PROCESS_EXIT_REQUESTED;
        __atomic_store_n(&current->process_exit_requested, true,
                         __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);

    /* A process exit is cooperative for siblings. Wake every registered wait
     * without holding the registry lock; syscall admission is already closed. */
    if (first_request) {
        for (kernel_thread_t *thread = process->threads; thread;
             thread = thread->process_next) {
            if (thread != current) {
                console_cancel_waiter(thread);
                (void)scheduler_wake_waiter(thread);
            }
        }
    }
    return true;
}

bool process_publish_launch(process_t *process)
{
    bool retry_cleanup;
    u64 flags;

    if (!process) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->launch_ready || !process->main_thread) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    /* The creator calls this only after all process resources are installed
     * and the thread is enqueued (or PID 1's image is fully prepared). */
    process->launch_ready = true;
    /* The initial thread is runnable before launch publication. It may finish
     * and its handoff notification may already have been consumed while the
     * cleanup worker correctly ignored this unpublished process. Capture the
     * lifecycle state in the same registry critical section as publication,
     * then reassert the worker event after dropping the lock. */
    retry_cleanup = process->state == PROCESS_STATE_EXITING;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (retry_cleanup && !process_cleanup_worker_wake(process))
        panic("process: launch publication could not notify cleanup worker");
    return true;
}

bool process_syscall_enter(process_t *process)
{
    bool admitted = false;
    u64 flags;

    if (!process) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state == PROCESS_STATE_ACTIVE &&
        !__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE) &&
        !process->termination_pending &&
        process->active_syscalls != ~(u32)0 &&
        object_reference(&process->object)) {
        process->active_syscalls++;
        admitted = true;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return admitted;
}

void process_syscall_leave(process_t *process)
{
    u64 flags;
    bool queue_cleanup;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (!process->active_syscalls)
        panic("process: syscall admission underflow");
    process->active_syscalls--;
    queue_cleanup = process->active_syscalls == 0U &&
        process->state == PROCESS_STATE_EXITING;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (queue_cleanup) process_cleanup_worker_wake(process);
    if (queue_cleanup)
        process_handle_reference_release(&process->object);
    else
        object_release(&process->object);
}

static bool process_termination_begin(process_t *process)
{
    u64 flags;
    bool admitted = false;

    if (!process) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state == PROCESS_STATE_ACTIVE && process->thread_count &&
        process->launch_ready && !process->active_syscalls &&
        !process->worker_reapers &&
        !process->termination_pending &&
        !process->external_termination_in_progress) {
        process->termination_pending = true;
        process->external_termination_in_progress = true;
        admitted = true;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return admitted;
}

static void process_termination_cancel(process_t *process)
{
    u64 flags;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    process->external_termination_in_progress = false;
    if (process->state == PROCESS_STATE_ACTIVE)
        process->termination_pending = false;
    spin_unlock_irqrestore(&process_registry_lock, flags);
}

bool process_terminate_current_exception(i32 status)
{
    process_t *process = process_current();
    kernel_thread_t *thread = thread_current();

    /* The IDT checked the saved privilege level.  Keep a second ownership
     * check here so an exception path can never terminate an unrelated
     * kernel/service thread. */
    if (!process || !thread || thread->process != process ||
        thread->process_state == THREAD_PROCESS_UNATTACHED ||
        !process_exit(process, status)) {
        return false;
    }
    return scheduler_terminate();
}

bool process_thread_exit_requested(const struct kernel_thread *thread)
{
    return thread && __atomic_load_n(&thread->process_exit_requested,
                                      __ATOMIC_ACQUIRE);
}

void process_thread_entry_completed(struct kernel_thread *thread, i32 status)
{
    process_t *process;

    if (!thread || !(process = thread->process)) return;
    if (thread->process_initial) {
        (void)process_exit(process, status);
        return;
    }
    if (!thread->worker_record) {
        /* Preserve completion accounting for existing process-owned kernel
         * threads. Native Ring 3 workers always carry a join record. */
        u64 flags = spin_lock_irqsave(&process_registry_lock);
        if (thread->process == process &&
            thread->process_state != THREAD_PROCESS_HANDOFF_COMPLETE) {
            process_storage_authorization_revoke_thread_locked(
                process, thread->id);
            thread->process_exit_status = status;
            thread->process_exit_status_valid = true;
            thread->process_state = THREAD_PROCESS_EXIT_REQUESTED;
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return;
    }
    process_worker_exit(thread, (u64)(i64)status);
}

void process_worker_exit(struct kernel_thread *raw_thread, u64 result)
{
    kernel_thread_t *thread = raw_thread;
    process_t *process;
    process_thread_record_t *record;
    u64 flags;

    if (!thread || !(process = thread->process) || thread->process_initial)
        return;
    flags = spin_lock_irqsave(&process_registry_lock);
    record = thread->worker_record;
    if (thread->process == process && record &&
        record->process == process && record->execution == thread &&
        thread->process_state != THREAD_PROCESS_HANDOFF_COMPLETE) {
        process_storage_authorization_revoke_thread_locked(
            process, thread->id);
        thread->process_exit_status = (i32)result;
        thread->process_exit_status_valid = true;
        record->result = result;
        thread->process_state = THREAD_PROCESS_EXIT_REQUESTED;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
}

static bool process_scheduler_handoff_complete_internal(
    struct kernel_thread *raw_thread, bool context_callback)
{
    kernel_thread_t *thread = raw_thread;
    process_t *process;
    kernel_thread_t *join_waiter = NULL;
    u64 flags;

    if (!thread || !(process = thread->process) ||
        (context_callback
            ? !scheduler_thread_handoff_callback_valid(thread)
            : !scheduler_thread_exit_complete(thread)))
        return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (context_callback &&
        (thread->process != process || !thread->process_reference_held)) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    if (context_callback) {
        if (process->scheduler_handoff_references == ~(u32)0) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: scheduler handoff reference overflow");
        }
        /* scheduler_context_switch_complete() retained one object reference
         * before this callback. Register its lifetime before the scheduler
         * clears context_switch_pending, so session/process collectors can
         * account for it even if the callback has not published it yet. */
        process->scheduler_handoff_references++;
    }
    if (thread->process == process &&
        thread->process_state != THREAD_PROCESS_HANDOFF_COMPLETE) {
        thread->process_state = THREAD_PROCESS_HANDOFF_COMPLETE;
        if (!process->live_thread_count) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: live thread count underflow");
        }
        process->live_thread_count--;
        if (thread->worker_record) {
            process_thread_record_t *record = thread->worker_record;
            if (record->process == process && record->execution == thread) {
                record->completed = true;
                record->kernel_stack_released = false;
                __atomic_store_n(&process->worker_reap_pending, true,
                                 __ATOMIC_RELEASE);
                /* Registration cancellation and event publication share the
                 * registry lock. Never export this scheduler pointer: a late
                 * wake after join cancellation could interrupt a later sleep,
                 * and a retained list reader would keep memory alive without
                 * making a terminated thread wakeable. */
                if (record->join_claimed != (record->join_waiter != NULL))
                    panic("process: inconsistent worker join registration");
                if (record->join_claimed) {
                    scheduler_wake_result_t wake_result;

                    join_waiter = record->join_waiter;
                    if (!record->listed || record->process != process ||
                        record->execution != thread || join_waiter == thread ||
                        join_waiter->process != process ||
                        !join_waiter->process_reference_held ||
                        (join_waiter->process_state != THREAD_PROCESS_ATTACHED &&
                         join_waiter->process_state !=
                             THREAD_PROCESS_EXIT_REQUESTED &&
                         join_waiter->process_state !=
                             THREAD_PROCESS_HANDOFF_COMPLETE) ||
                        !process_thread_member_locked(process, join_waiter))
                        panic("process: worker joiner lost process ownership");

                    /* Lock order is process_registry_lock -> scheduler_lock.
                     * The scheduler operation only latches or queues a wake;
                     * it cannot sleep, switch, reap, or release references. */
                    wake_result = scheduler_wake_waiter_result(join_waiter);
                    if (wake_result == SCHEDULER_WAKE_ALREADY_TERMINATED) {
                        /* Keep the completion result and join right durable;
                         * a later caller may claim the completed record. */
                        if (!record->join_claimed ||
                            record->join_waiter != join_waiter)
                            panic("process: worker join claim changed");
                        record->join_claimed = false;
                        record->join_waiter = NULL;
                    } else if (wake_result != SCHEDULER_WAKE_DELIVERED) {
                        panic("process: failed to wake completed worker joiner");
                    }
                }
            }
        }
    }
    (void)process_publish_terminated_locked(process);
    spin_unlock_irqrestore(&process_registry_lock, flags);

    return true;
}

void process_scheduler_handoff_complete(struct kernel_thread *thread)
{
    (void)process_scheduler_handoff_complete_internal(thread, false);
}

bool process_scheduler_context_handoff_complete(struct kernel_thread *thread)
{
    return process_scheduler_handoff_complete_internal(thread, true);
}

void process_scheduler_post_handoff(process_t *process)
{
    u32 pending;

    if (!process) return;
    /* Transfer the retained reference to the existing cleanup worker. This
     * bounded atomic publication avoids taking the process registry lock
     * while the incoming mutex continuation may still own a handoff marker. */
    pending = __atomic_fetch_add(
        &process->scheduler_handoff_releases_pending, 1U, __ATOMIC_RELEASE);
    if (pending == ~(u32)0)
        panic("process: scheduler handoff release count overflow");
    process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP | PROCESS_CLEANUP_EXIT);
    if (!process_cleanup_worker_wake(process))
        panic("process: scheduler handoff before cleanup worker startup");
}

static void process_unlink_locked(process_t *process)
{
    process_t **cursor;
    if (!process || !process->parent) return;
    cursor = &process->parent->first_child;
    while (*cursor && *cursor != process) cursor = &(*cursor)->next_sibling;
    if (*cursor == process) *cursor = process->next_sibling;
    process->parent = NULL;
    process->next_sibling = NULL;
}

static void process_unlink_all_locked(process_t *process)
{
    process_t **cursor;

    if (!process) return;
    cursor = &all_processes;
    while (*cursor && *cursor != process) cursor = &(*cursor)->next_all;
    if (*cursor == process) *cursor = process->next_all;
    process->next_all = NULL;
}

static void process_unlink(process_t *process)
{
    u64 flags;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    process_unlink_locked(process);
    spin_unlock_irqrestore(&process_registry_lock, flags);
}

static void process_unlink_all(process_t *process)
{
    u64 flags;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    process_unlink_all_locked(process);
    spin_unlock_irqrestore(&process_registry_lock, flags);
}

static void process_abort(process_t *process)
{
    u64 flags;
    kernel_thread_t *thread;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state != PROCESS_STATE_ACTIVE || process->launch_ready ||
        process->active_syscalls || process->thread_count != 1U ||
        process->live_thread_count != 1U || !process->main_thread) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        panic("process: invalid launch rollback state");
    }
    thread = process->main_thread;
    process->state = PROCESS_STATE_EXITING;
    process->termination_pending = true;
    process_storage_authorization_clear_locked(process);
    __atomic_store_n(&process->exit_requested, true, __ATOMIC_RELEASE);
    __atomic_store_n(&thread->process_exit_requested, true, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&process_registry_lock, flags);

    /* Launch rollback owns a suspended, unpublished initial thread. Mark it
     * stopped through the scheduler before unlinking the process-owned record. */
    if (!scheduler_terminate_thread(thread))
        panic("process: launch rollback thread was already active");
    process_scheduler_handoff_complete(thread);

    flags = spin_lock_irqsave(&process_registry_lock);
    process->state = PROCESS_STATE_TERMINATED;
    process->termination_pending = false;
    process_unlink_locked(process);
    process_unlink_all_locked(process);
    spin_unlock_irqrestore(&process_registry_lock, flags);
    terminal_output_finish_for_process(process->pid,
                                       &process->terminal_output);
    (void)terminal_alternate_leave_process(process->pid);
    ipc_process_exit(process);
    process_handle_close_all(process);
    process_cwd_clear(process);
    if (!process_detach_thread(process, thread) || !thread_destroy(thread))
        panic("process: launch rollback could not release initial thread");
    process->owner_reference_held = false;
    object_release(&process->object);
}

bool process_terminate_external(process_t *process, i32 status)
{
    kernel_thread_t *threads = NULL;
    usize thread_count = 0;
    bool exiting_process = false;
    bool reserved = false;
    bool thread_list_pinned = false;
    u64 flags;

    if (!process || process == process_current()) {
        return false;
    }
    flags = spin_lock_irqsave(&process_registry_lock);
    if (!process->launch_ready) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    exiting_process = process->state == PROCESS_STATE_EXITING;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (!exiting_process) {
        if (!process_termination_begin(process)) return false;
        flags = spin_lock_irqsave(&process_registry_lock);
        if (process->state == PROCESS_STATE_ACTIVE &&
            process->termination_pending &&
            process->external_termination_in_progress &&
            process_thread_list_read_acquire_locked(process)) {
            threads = process->threads;
            thread_count = process->thread_count;
            thread_list_pinned = true;
            reserved = true;
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!reserved) {
            process_termination_cancel(process);
            return false;
        }
    } else {
        flags = spin_lock_irqsave(&process_registry_lock);
        if (process->state == PROCESS_STATE_EXITING &&
            !process->active_syscalls && !process->worker_reapers &&
            process->thread_count &&
            !process->external_termination_in_progress &&
            process_thread_list_read_acquire_locked(process)) {
            process->external_termination_in_progress = true;
            threads = process->threads;
            thread_count = process->thread_count;
            thread_list_pinned = true;
            reserved = true;
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!reserved) return false;
    }

    if (!scheduler_terminate_thread_group(threads, thread_count)) {
        if (thread_list_pinned) process_thread_list_read_release(process);
        process_termination_cancel(process);
        return false;
    }

    flags = spin_lock_irqsave(&process_registry_lock);
    /* A cooperative exit may finish while the group preflight is running.
     * Do not republish termination_pending after cleanup has already made the
     * process reapable. The list pin keeps scheduler records valid until this
     * operation stops traversing them. */
    if (process->state == PROCESS_STATE_TERMINATED) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (thread_list_pinned) process_thread_list_read_release(process);
        return true;
    }
    if (process->state == PROCESS_STATE_ACTIVE) {
        process->state = PROCESS_STATE_EXITING;
        process_storage_authorization_clear_locked(process);
        __atomic_store_n(&process->exit_requested, true, __ATOMIC_RELEASE);
        process->exit_status = status;
    }
    process->termination_pending = true;
    process->external_termination_in_progress = false;
    for (kernel_thread_t *thread = process->threads; thread;
         thread = thread->process_next) {
        __atomic_store_n(&thread->process_exit_requested, true,
                         __ATOMIC_RELEASE);
        if (thread->process_state == THREAD_PROCESS_ATTACHED)
            thread->process_state = THREAD_PROCESS_EXIT_REQUESTED;
    }
    bool cleanup_ready = process_thread_list_complete_locked(process);
    spin_unlock_irqrestore(&process_registry_lock, flags);

    for (kernel_thread_t *thread = process->threads; thread;
         thread = thread->process_next) {
        console_cancel_waiter(thread);
        (void)scheduler_wake_waiter(thread);
    }
    if (thread_list_pinned) process_thread_list_read_release(process);
    if (cleanup_ready) (void)process_cleanup_exit_resources(process);
    flags = spin_lock_irqsave(&process_registry_lock);
    bool terminated = process->state == PROCESS_STATE_TERMINATED;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return terminated;
}

bool process_terminate_session_members(mg_session_id_t session_id)
{
    bool result = true;

    if (session_id == PROCESS_NO_SESSION) return false;
    for (;;) {
        process_t *process = NULL;
        u64 flags = spin_lock_irqsave(&process_registry_lock);

        for (process = all_processes; process; process = process->next_all) {
            if (process->session_id == session_id &&
                (process->state == PROCESS_STATE_ACTIVE ||
                 process->state == PROCESS_STATE_EXITING) &&
                object_reference(&process->object)) {
                break;
            }
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!process) break;
        if (!process_terminate_external(process, -1)) {
            result = false;
            process_handle_reference_release(&process->object);
            break;
        }
        process_handle_reference_release(&process->object);
    }
    return result;
}

void process_reap_session_members(mg_session_id_t session_id)
{
    for (;;) {
        process_t *process = NULL;
        u64 flags = spin_lock_irqsave(&process_registry_lock);

        for (process = all_processes; process; process = process->next_all) {
            if (process->launch_ready &&
                process->session_id == session_id &&
                process->state == PROCESS_STATE_TERMINATED &&
                process->owner_reference_held &&
                process_session_collection_ready_locked(process) &&
                /* Transferred process references remain in ref_count and
                 * intentionally keep this exact-count gate closed. */
                __atomic_load_n(&process->object.ref_count, __ATOMIC_ACQUIRE) ==
                    (u64)process->thread_count + 1U +
                        process->scheduler_handoff_references &&
                object_reference(&process->object))
                break;
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!process) break;

        if (!process_finalize_exit(process)) {
            object_release(&process->object);
            break;
        }
        flags = spin_lock_irqsave(&process_registry_lock);
        bool reap = process->owner_reference_held &&
            process->state == PROCESS_STATE_TERMINATED &&
            process->thread_count == 0 &&
            __atomic_load_n(&process->object.ref_count, __ATOMIC_ACQUIRE) ==
                2U + process->scheduler_handoff_references;
        if (reap) {
            process_unlink_locked(process);
            process->owner_reference_held = false;
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!reap) {
            object_release(&process->object);
            break;
        }
        object_release(&process->object); /* process owner reference */
        object_release(&process->object);
    }
}

/* Release a retained process-object handle reference without permitting a
 * potentially final drop in an unsafe caller context. A copied session ID
 * lets safe release or the worker retry collection after the reference is
 * gone. This also covers in-flight handle operations overlapping close. */
void process_handle_reference_release(kernel_object_t *object)
{
    mg_session_id_t session_id = PROCESS_NO_SESSION;

    if (!object) return;
    if (object->type != OBJECT_TYPE_PROCESS) {
        object_release(object);
        return;
    }

    process_t *target = object_process(object);
    bool safe_to_release = process_cleanup_context_valid();
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    /* Capture stable identity while the caller's retained reference pins the
     * object. Collection eligibility may change before either safe release. */
    if (target->launch_ready && target->owner_reference_held &&
        target->session_id != PROCESS_NO_SESSION)
        session_id = target->session_id;

    if (!safe_to_release) {
        u32 pending = __atomic_load_n(
            &target->deferred_reference_releases_pending, __ATOMIC_RELAXED);
        if (pending == ~(u32)0) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: deferred reference count overflow");
        }
        if (__atomic_load_n(&target->object.ref_count, __ATOMIC_ACQUIRE) <=
            pending) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            panic("process: deferred reference has no retained owner");
        }
        __atomic_store_n(&target->deferred_reference_releases_pending,
                         pending + 1U, __ATOMIC_RELEASE);
        if (session_id != PROCESS_NO_SESSION)
            target->deferred_reference_collection_retry = true;
        spin_unlock_irqrestore(&process_registry_lock, flags);

        /* The actual retained object reference remains live in the pending
         * count. This durable worker-pass latch also covers publication before
         * worker readiness and closes the worker scan-to-park race. */
        process_cleanup_worker_request_session_collection_retry();
        return;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);

    /* In safe contexts, preserve ordinary reference-release semantics. */
    object_release(object);
    if (session_id != PROCESS_NO_SESSION)
        process_reap_session_members(session_id);
}

mg_session_id_t process_session_id(const process_t *process)
{
    return process ? process->session_id : PROCESS_NO_SESSION;
}

bool process_is_session_shell(const process_t *process)
{
    return process && process->session_shell;
}

bool process_take_interrupt(process_t *process)
{
    if (!process) return false;
    /* Process exit is persistent and must interrupt every blocked sibling;
     * Ctrl+C remains a consumable, one-shot process request. */
    if (__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE))
        return true;
    return __atomic_exchange_n(&process->interrupt_requested,
                                false, __ATOMIC_ACQ_REL);
}

bool process_interrupt_foreground_session(void)
{
    process_t *shell = NULL;
    process_t *target = NULL;
    usize process_count = 0;
    u64 flags = spin_lock_irqsave(&process_registry_lock);

    for (process_t *process = all_processes; process;
         process = process->next_all) {
        process_count++;
        if (!process->launch_ready ||
            process->state != PROCESS_STATE_ACTIVE ||
            !process->session_shell || !process->waiting_child ||
            process->waiting_child->state != PROCESS_STATE_ACTIVE)
            continue;
        /* The console has one active login session. If that invariant is ever
         * extended to multiple simultaneous sessions, keyboard ownership
         * must identify the session instead of guessing here. */
        if (shell) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            return false;
        }
        shell = process;
    }

    if (shell) {
        process_t *current = shell;
        for (usize depth = 0; depth < process_count; depth++) {
            process_t *child = current->waiting_child;
            if (!child) break;
            if (child->parent != current ||
                child->session_id != shell->session_id ||
                child->state != PROCESS_STATE_ACTIVE) {
                current = NULL;
                break;
            }
            current = child;
        }
        if (current && current != shell &&
            process_thread_list_read_acquire_locked(current)) {
            if (object_reference(&current->object)) {
                target = current;
                __atomic_store_n(&target->interrupt_requested, true,
                                 __ATOMIC_RELEASE);
            } else {
                current->thread_list_readers--;
            }
        }
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (!target) return false;

    /* A blocked operation observes the one-shot request after this wake. A
     * running process will observe it at its next interruptible operation. */
    for (kernel_thread_t *thread = target->threads; thread;
         thread = thread->process_next)
        (void)scheduler_wake_waiter(thread);
    process_thread_list_read_release(target);
    process_handle_reference_release(&target->object);
    return true;
}

bool process_shutdown_requested(const process_t *process)
{
    return process && __atomic_load_n(&process->shutdown_requested,
                                      __ATOMIC_ACQUIRE);
}

bool process_shutdown_pending(void)
{
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    bool pending = process_shutdown_active;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return pending;
}

static bool process_matches_shutdown_phase(const process_t *process,
                                           const process_t *requester,
                                           bool services)
{
    if (!process || process == requester) return false;
    /* A process that was already under construction when shutdown started is
     * a quiescence dependency even before its service/session metadata is
     * published. The caller waits for publication, then classifies it. */
    if (!process->launch_ready) return true;
    if (!services) return !process->system_service;
    /* PID 1 and the session coordinators remain alive through the final
     * transition. Sprout restarts leaf services if sync/timeout aborts; the
     * session owners must not retire a session containing the shutdown caller. */
    return process->system_service &&
        process->service_id != MG_SERVICE_SPROUT &&
        process->service_id != MG_SERVICE_SESSIOND &&
        process->service_id != MG_SERVICE_LOGIND;
}

static bool process_in_shutdown_phase(const process_t *process,
                                      const process_t *requester,
                                      bool services)
{
    return process_matches_shutdown_phase(process, requester, services) &&
           process->state == PROCESS_STATE_ACTIVE;
}

static void process_shutdown_mark_phase(process_t *requester, bool services)
{
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    for (process_t *process = all_processes; process;
         process = process->next_all) {
        if (process_in_shutdown_phase(process, requester, services)) {
            __atomic_store_n(&process->shutdown_requested, true,
                             __ATOMIC_RELEASE);
            process->shutdown_wake_sent = false;
        }
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);

    /* Wake each process-owned waiter without holding the registry lock across
     * scheduler operations. The process reference pins every list member. */
    for (;;) {
        process_t *target = NULL;

        flags = spin_lock_irqsave(&process_registry_lock);
        for (process_t *process = all_processes; process;
             process = process->next_all) {
            if (process_in_shutdown_phase(process, requester, services) &&
                process->shutdown_requested && !process->shutdown_wake_sent &&
                process_thread_list_read_acquire_locked(process)) {
                if (object_reference(&process->object)) {
                    process->shutdown_wake_sent = true;
                    target = process;
                    break;
                }
                process->thread_list_readers--;
            }
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!target) break;
        /* The target may still be running between registering its wait and
         * entering scheduler_block(). Publish a pending wake in that state so
         * the shutdown result cannot be lost in the check-to-sleep window. */
        for (kernel_thread_t *thread = target->threads; thread;
             thread = thread->process_next)
            (void)scheduler_wake_waiter(thread);
        process_thread_list_read_release(target);
        process_handle_reference_release(&target->object);
    }
}

static process_t *process_shutdown_next_work(process_t *requester,
                                             bool services)
{
    process_t *target = NULL;
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    for (process_t *process = all_processes; process;
         process = process->next_all) {
        bool active = process_in_shutdown_phase(process, requester, services);
        bool exiting = process_matches_shutdown_phase(process, requester,
                                                       services) &&
            process->state == PROCESS_STATE_EXITING;
        if ((active || exiting) &&
            object_reference(&process->object)) {
            target = process;
            break;
        }
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return target;
}

static int process_shutdown_wait_phase(process_t *requester, bool services)
{
    u64 deadline = timer_uptime_ms() + PROCESS_SHUTDOWN_GRACE_MS;
    u64 force_deadline = 0;
    bool forcing = false;

    for (;;) {
        process_t *target = process_shutdown_next_work(requester, services);
        if (!target) return MG_OK;
        u64 now = timer_uptime_ms();
        if (now >= deadline && !forcing) {
            forcing = true;
            force_deadline = now + PROCESS_SHUTDOWN_FORCE_MS;
            kprint("[WARN] shutdown grace period expired; terminating remaining %s processes\n",
                   services ? "service" : "application");
        }
        if (forcing) {
            (void)process_terminate_external(target, -1);
            process_handle_reference_release(&target->object);
            if (timer_uptime_ms() >= force_deadline) {
                process_t *remaining = process_shutdown_next_work(
                    requester, services);
                if (remaining) {
                    kprint("[ERROR] shutdown timeout waiting for safe process exit pid=%llu name=%s\n",
                           remaining->pid, remaining->name);
                    process_handle_reference_release(&remaining->object);
                }
                return MG_ERR_TIMEOUT;
            }
        } else {
            process_handle_reference_release(&target->object);
        }
        (void)scheduler_sleep(1U);
    }
}

int process_shutdown_quiesce(process_t *requester)
{
    int result;
    u64 flags;

    if (!requester || requester->state != PROCESS_STATE_ACTIVE)
        return MG_ERR_BAD_ARGUMENT;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process_shutdown_active) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return MG_ERR_BUSY;
    }
    process_shutdown_active = true;
    spin_unlock_irqrestore(&process_registry_lock, flags);

    /* Applications, including session shells, get the first cleanup window.
     * Services stay alive until their clients have exited. */
    process_shutdown_mark_phase(requester, false);
    result = process_shutdown_wait_phase(requester, false);
    if (result != MG_OK) goto fail;

    /* Stop leaf services after sessions. Sprout and session coordinators stay
     * alive as the control plane through sync and the hardware transition. */
    process_shutdown_mark_phase(requester, true);
    result = process_shutdown_wait_phase(requester, true);
    if (result != MG_OK) goto fail;
    return MG_OK;

fail:
    process_shutdown_abort();
    return result;
}

void process_shutdown_abort(void)
{
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    process_shutdown_active = false;
    for (process_t *process = all_processes; process;
         process = process->next_all) {
        __atomic_store_n(&process->shutdown_requested, false,
                         __ATOMIC_RELEASE);
        process->shutdown_wake_sent = false;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
}

typedef process_arguments_t process_args_t;

static bool parse_spawn_cmdline(const char *cmdline, char *bin_path,
                                usize bin_path_size, process_args_t *args)
{
    char raw[PROCESS_COMMAND_LINE_CAPACITY];
    const char *argv[PROCESS_COMMAND_LINE_CAPACITY / 2U + 1U];
    usize argc = 0;
    if (!cmdline || !args || strlen(cmdline) >= sizeof(raw)) return false;
    strcpy(raw, cmdline);
    char *cursor = raw;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    while (*cursor) {
        char *write = cursor;
        char quote = '\0';
        if (argc == sizeof(argv) / sizeof(argv[0])) return false;
        argv[argc++] = cursor;
        while (*cursor) {
            if (quote) {
                if (*cursor == quote) { quote = '\0'; cursor++; }
                else *write++ = *cursor++;
            } else if (*cursor == '\'' || *cursor == '"') quote = *cursor++;
            else if (*cursor == ' ' || *cursor == '\t') break;
            else *write++ = *cursor++;
        }
        if (quote) return false;
        if (*cursor) cursor++;
        *write = '\0';
        while (*cursor == ' ' || *cursor == '\t') cursor++;
    }
    if (!argc || !argv[0][0] || strlen(argv[0]) >= bin_path_size) return false;
    strcpy(bin_path, argv[0]);
    return process_arguments_collect(args, (uintptr_t)argv, argc, NULL) == MG_OK;
}

static int setup_user_stack_args(process_t *process, const process_args_t *args)
{
    if (!process || !args || !args->argc ||
        args->argc > MG_PROCESS_MAX_ARGUMENTS || !args->offsets || !args->first)
        return VFS_ERR_INVALID_PARAM;
    usize vector_bytes = (args->argc + 1U) * sizeof(uintptr_t);
    if (vector_bytes > MG_PROCESS_ARGUMENT_BYTES_MAX ||
        args->bytes > MG_PROCESS_ARGUMENT_BYTES_MAX - vector_bytes)
        return VFS_ERR_INVALID_PARAM;
    usize total = vector_bytes + args->bytes;
    usize page_count = (total + VMM_PAGE_SIZE - 1U) / VMM_PAGE_SIZE;
    process_argument_chunk_t *chunk = args->first;
    usize chunk_offset = 0;
    usize mapped = 0;
    for (; mapped < page_count; mapped++) {
        phys_addr_t frame = pmm_alloc_frame();
        if (!frame) break;
        u8 *memory = phys_to_virt(frame);
        if (!memory || !vmm_direct_map_valid(frame))
            panic("process: argument frame lacks direct mapping");
        memset(memory, 0, VMM_PAGE_SIZE);
        usize position = mapped * VMM_PAGE_SIZE;
        usize in_page = 0;
        /* Pointer slots are aligned and cannot cross a physical page. */
        while (in_page < VMM_PAGE_SIZE && position < vector_bytes) {
            usize index = position / sizeof(uintptr_t);
            uintptr_t value = index == args->argc ? 0 :
                PROCESS_USER_ARGUMENT_BASE + vector_bytes + args->offsets[index];
            memcpy(memory + in_page, &value, sizeof(value));
            in_page += sizeof(value);
            position += sizeof(value);
        }
        while (in_page < VMM_PAGE_SIZE && position < total) {
            if (!chunk) panic("process: incomplete argument snapshot");
            usize count = chunk->used - chunk_offset;
            if (count > VMM_PAGE_SIZE - in_page) count = VMM_PAGE_SIZE - in_page;
            memcpy(memory + in_page, chunk->data + chunk_offset, count);
            in_page += count;
            position += count;
            chunk_offset += count;
            if (chunk_offset == chunk->used) { chunk = chunk->next; chunk_offset = 0; }
        }
        if (!vmm_map_user_page(process->address_space,
                (void *)(PROCESS_USER_ARGUMENT_BASE + mapped * VMM_PAGE_SIZE),
                frame, PTE_USER | PTE_READWRITE | PTE_NX)) {
            pmm_free_frame(frame);
            break;
        }
    }
    if (mapped != page_count) {
        while (mapped) {
            phys_addr_t frame;
            mapped--;
            if (!vmm_unmap_user_page(process->address_space,
                    (void *)(PROCESS_USER_ARGUMENT_BASE + mapped * VMM_PAGE_SIZE),
                    &frame)) panic("process: argument rollback failed");
            pmm_free_frame(frame);
        }
        return VFS_ERR_NO_MEM;
    }
    /* Publication happens only after every initialized page is owned by the
     * inactive child address space. Its ordinary destructor releases them. */
    process->user_stack_sp = process->user_stack_top - 16U;
    process->user_argc = args->argc;
    process->user_argv = PROCESS_USER_ARGUMENT_BASE;
    return VFS_OK;
}

static int process_arguments_executable_path(const process_args_t *args,
                                              char **out_path)
{
    usize length = 0;
    bool terminated = false;
    char *path;

    if (!args || !out_path || !args->argc || !args->offsets ||
        !args->first || args->offsets[0] != 0U) return VFS_ERR_INVALID_PARAM;
    *out_path = NULL;
    for (process_argument_chunk_t *chunk = args->first; chunk && !terminated;
         chunk = chunk->next) {
        for (usize i = 0; i < chunk->used; i++) {
            if (!chunk->data[i]) { terminated = true; break; }
            if (length == MG_PATH_MAX_BYTES) return VFS_ERR_NAME_TOO_LONG;
            length++;
        }
    }
    if (!terminated || !length) return VFS_ERR_INVALID_PARAM;
    path = kmalloc(length + 1U);
    if (!path) return VFS_ERR_NO_MEM;
    usize copied = 0;
    terminated = false;
    for (process_argument_chunk_t *chunk = args->first;
         chunk && !terminated; chunk = chunk->next) {
        for (usize i = 0; i < chunk->used; i++) {
            path[copied++] = (char)chunk->data[i];
            if (!chunk->data[i]) { terminated = true; break; }
        }
    }
    if (!terminated || copied != length + 1U) {
        kfree(path);
        return VFS_ERR_INVALID_PARAM;
    }
    *out_path = path;
    return VFS_OK;
}

bool process_setup_argv(process_t *process, const char *const *argv,
                        u32 argc)
{
    process_args_t args;
    if (!process || process_arguments_collect(&args, (uintptr_t)argv, argc,
                                               NULL) != MG_OK)
        return false;
    int result = setup_user_stack_args(process, &args);
    process_arguments_destroy(&args);
    if (result != VFS_OK) return false;
    return !process->image_loaded ||
        process_user_context_prepare_initial(process);
}

static process_user_region_t *process_user_tls_create(process_t *process,
                                                       uintptr_t *out_fs_base)
{
    usize alignment;
    usize module_size;
    usize slack;
    usize required;
    usize mapped_pages;
    process_user_region_t *region;
    uintptr_t tls_start;

    if (out_fs_base) *out_fs_base = 0;
    if (!process || !process->tls_present || !process->tls_memory_size ||
        !process->tls_alignment ||
        !process_size_align_up(process->tls_memory_size,
                               process->tls_alignment, &module_size))
        return NULL;
    alignment = process->tls_alignment;
    slack = alignment - 1U;
    if (module_size > ~(usize)0 - slack ||
        module_size + slack > ~(usize)0 - PROCESS_TLS_TCB_BYTES)
        return NULL;
    required = module_size + slack + PROCESS_TLS_TCB_BYTES;
    if (!process_memory_size_to_pages(required, &mapped_pages)) return NULL;
    region = process_user_region_create(process,
        PROCESS_USER_REGION_THREAD_TLS, 1U, mapped_pages, alignment,
        process->tls_image, process->tls_file_size, 0U, module_size,
        &tls_start);
    if (!region) return NULL;
    if (tls_start > VMM_USER_ADDRESS_LIMIT - module_size) {
        if (process_user_region_release(process, region) ==
            PROCESS_REGION_RELEASE_INVALID)
            panic("process: TLS range rollback failed");
        return NULL;
    }
    if (out_fs_base) *out_fs_base = tls_start + module_size;
    return region;
}

bool process_user_context_prepare_initial(process_t *process)
{
    kernel_thread_t *thread;
    user_execution_context_t *context;
    process_user_region_t *stack_region;
    process_user_region_t *tls_region = NULL;
    uintptr_t fs_base = 0;

    if (!process || !process->image_loaded || !process->main_thread ||
        !process->entry_point || !process->user_stack_sp ||
        !process->user_stack_top || !process->user_argc || !process->user_argv)
        return false;
    thread = process->main_thread;
    if (thread->process != process || !thread->process_initial ||
        thread->user_context.valid)
        return false;

    stack_region = process_user_region_find(process,
                                  PROCESS_USER_REGION_INITIAL_STACK);
    if (!stack_region) return false;
    if (process->tls_present) {
        tls_region = process_user_tls_create(process, &fs_base);
        if (!tls_region) return false;
    }

    context = &thread->user_context;
    memset(context, 0, sizeof(*context));
    context->instruction_pointer = process->entry_point;
    context->stack_pointer = process->user_stack_sp;
    context->rdi = process->user_argc;
    context->rsi = process->user_argv;
    context->fs_base = fs_base;
    context->stack_region = stack_region;
    context->tls_region = tls_region;
    thread->user_fs_base = fs_base;
    __atomic_store_n(&context->valid, true, __ATOMIC_RELEASE);
    return true;
}

bool process_user_context_prepare_worker(process_t *process,
                                         struct kernel_thread *thread,
                                         uintptr_t entry, uintptr_t argument)
{
    process_user_region_t *stack_region = NULL;
    process_user_region_t *tls_region = NULL;
    user_execution_context_t context;
    uintptr_t fs_base = 0;
    uintptr_t stack_top;
    uintptr_t stack_pointer;
    usize stack_bytes = PROCESS_USER_STACK_PAGES * VMM_PAGE_SIZE;
    uintptr_t return_address = PROCESS_USER_THREAD_RETURN_ADDRESS;

    if (!process || !thread || !process->image_loaded || !process->address_space ||
        process->state != PROCESS_STATE_ACTIVE ||
        __atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE) ||
        thread->process || thread->process_state != THREAD_PROCESS_UNATTACHED ||
        thread->user_context.valid || !entry ||
        !vmm_user_range_executable(process->address_space, entry, 1U) ||
        !vmm_user_range_executable(process->address_space,
            PROCESS_USER_THREAD_RETURN_ADDRESS, 1U))
        return false;

    /* Every allocation is private to this unpublished context. Nothing is
     * attached to the process thread list or scheduler until the record is
     * complete and its valid flag is released below. */
    stack_region = process_user_region_create(process,
        PROCESS_USER_REGION_WORKER_STACK, 1U, PROCESS_USER_STACK_PAGES,
        VMM_PAGE_SIZE, &return_address, sizeof(return_address),
        stack_bytes - sizeof(return_address), 0U, NULL);
    if (!stack_region) return false;
    stack_top = stack_region->mapping_base + stack_bytes;
    stack_pointer = stack_top - sizeof(return_address);

    if (process->tls_present) {
        tls_region = process_user_tls_create(process, &fs_base);
        if (!tls_region) {
            if (process_user_region_release(process, stack_region) ==
                PROCESS_REGION_RELEASE_INVALID)
                panic("process: worker-stack rollback failed");
            process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);
            return false;
        }
    }

    /* Process exit may race an admitted creator while its private mappings
     * are being prepared. Keep this context unpublished and roll back now;
     * process_attach_thread() remains the final serialized admission check
     * for an exit racing after this point. */
    if (process->state != PROCESS_STATE_ACTIVE ||
        __atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE)) {
        if (tls_region && process_user_region_release(process, tls_region) ==
                PROCESS_REGION_RELEASE_INVALID)
            panic("process: worker-TLS termination rollback failed");
        if (process_user_region_release(process, stack_region) ==
                PROCESS_REGION_RELEASE_INVALID)
            panic("process: worker-stack termination rollback failed");
        process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);
        return false;
    }

    memset(&context, 0, sizeof(context));
    context.instruction_pointer = entry;
    context.stack_pointer = stack_pointer;
    context.rdi = argument;
    context.fs_base = fs_base;
    context.stack_region = stack_region;
    context.tls_region = tls_region;
    context.valid = false;
    thread->entry = process_user_thread_entry;
    thread->entry_argument = process;
    thread->user_fs_base = fs_base;
    thread->user_context = context;
    __atomic_store_n(&thread->user_context.valid, true, __ATOMIC_RELEASE);
    return true;
}

bool process_user_context_discard_worker(process_t *process,
                                         struct kernel_thread *thread)
{
    process_user_region_t *stack_region;
    process_user_region_t *tls_region;

    if (!process || !thread || thread->process ||
        thread->process_state != THREAD_PROCESS_UNATTACHED ||
        !__atomic_load_n(&thread->user_context.valid, __ATOMIC_ACQUIRE))
        return false;
    stack_region = thread->user_context.stack_region;
    tls_region = thread->user_context.tls_region;
    if (!stack_region || stack_region->kind != PROCESS_USER_REGION_WORKER_STACK)
        return false;
    if (!process_user_region_set_worker_owned(process, stack_region,
                                              tls_region, false))
        return false;
    if (tls_region &&
        (tls_region->kind != PROCESS_USER_REGION_THREAD_TLS ||
         process_user_region_release(process, tls_region) ==
             PROCESS_REGION_RELEASE_INVALID))
        return false;
    if (process_user_region_release(process, stack_region) ==
        PROCESS_REGION_RELEASE_INVALID)
        return false;
    memset(&thread->user_context, 0, sizeof(thread->user_context));
    thread->user_fs_base = 0;
    thread->entry = NULL;
    thread->entry_argument = NULL;
    process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);
    return true;
}

int process_thread_create(process_t *process, uintptr_t entry,
                          uintptr_t argument, process_handle_t *out_handle,
                          u64 *out_thread_id)
{
    kernel_thread_t *thread = NULL;
    process_thread_record_t *record = NULL;
    process_handle_t handle = 0;
    bool handle_reserved = false;
    bool handle_reference = false;
    bool attached = false;
    bool published = false;
    bool release_process_reference = false;
    int rollback_error = MG_ERR_NO_MEMORY;
    u64 thread_id;
    u64 flags;

    if (!process || !entry || !out_handle || !out_thread_id)
        return MG_ERR_BAD_ARGUMENT;
    if (__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE))
        return MG_ERR_SHUTDOWN;
    if (!vmm_user_range_executable(process->address_space, entry, 1U))
        return MG_ERR_INVALID_EXEC;

    thread = thread_create_suspended("user-worker", process_user_thread_entry,
                                     process);
    if (!thread) return MG_ERR_NO_MEMORY;
    record = (process_thread_record_t *)kmalloc(sizeof(*record));
    if (!record) {
        if (!thread_destroy(thread))
            panic("process: failed to destroy uninitialized worker");
        return MG_ERR_NO_MEMORY;
    }
    memset(record, 0, sizeof(*record));
    object_init(&record->object, OBJECT_TYPE_THREAD,
                process_thread_record_destroy);
    thread->worker_record = record;

    if (!process_user_context_prepare_worker(process, thread, entry,
                                              argument)) {
        thread->worker_record = NULL;
        if (thread->user_context.valid &&
            !process_user_context_discard_worker(process, thread))
            panic("process: worker context preparation rollback failed");
        if (!thread_destroy(thread))
            panic("process: failed to destroy failed worker context");
        object_release(&record->object);
        return !__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE)
            ? MG_ERR_NO_MEMORY : MG_ERR_SHUTDOWN;
    }

    record->execution = thread;
    record->id = thread->id;
    /* The completion record may be detached and reclaimed as soon as the
     * handle becomes visible to a sibling. Keep the returned identity in
     * creator-owned storage before publishing either record or handle. */
    thread_id = record->id;
    record->stack_region = thread->user_context.stack_region;
    record->tls_region = thread->user_context.tls_region;
    if (!process_user_region_set_worker_owned(process,
            record->stack_region, record->tls_region, true))
        goto rollback_private;

    /* The initial reference becomes process-list ownership on commit.  A
     * second reference is transferred into the hidden, reserved handle. */
    if (!object_reference(&record->object)) goto rollback_private;
    handle_reference = true;
    if (!process_handle_reserve(process, &record->object, 0, &handle))
        goto rollback_private;
    handle_reserved = true;
    handle_reference = false; /* transferred to the handle table */

    /* Failure to reserve a handle is resource exhaustion; after reservation,
     * failure to admit or enqueue the fully prepared worker is a lifecycle or
     * scheduler-busy result instead. */
    rollback_error = MG_ERR_BUSY;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process_thread_register_locked(process, thread, false)) {
        attached = true;
        record->process = process;
        record->listed = true;
        record->next = process->thread_records;
        process->thread_records = record;
        thread->worker_record = record;
        /* Keep the handle unavailable until ownership and queue publication
         * are ready.  Termination admission shares this lock, so an exit sees
         * either an unpublished private context or a complete worker. */
        published = process_thread_handle_activate_locked(process, handle) &&
                    scheduler_enqueue(thread);
        if (!published) {
            if (process_thread_handle_deactivate_locked(process, handle))
                handle_reserved = true;
            process->thread_records = record->next;
            record->next = NULL;
            record->listed = false;
            record->process = NULL;
            thread->worker_record = NULL;
            process->threads = thread->process_next;
            process->thread_count--;
            process->live_thread_count--;
            thread->process = NULL;
            thread->process_next = NULL;
            thread->process_state = THREAD_PROCESS_UNATTACHED;
            thread->process_reference_held = false;
            release_process_reference = true;
            attached = false;
        }
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);

    if (published) {
        *out_handle = handle;
        *out_thread_id = thread_id;
        return MG_OK;
    }
    if (release_process_reference) object_release(&process->object);
    if (handle_reserved && !process_handle_cancel(process, handle))
        panic("process: failed to cancel unpublished worker handle");
    if (attached)
        panic("process: worker publication rollback remained attached");

rollback_private:
    if (handle_reference) object_release(&record->object);
    if (thread->user_context.valid &&
        !process_user_context_discard_worker(process, thread))
        panic("process: private worker rollback failed");
    thread->worker_record = NULL;
    if (!thread_destroy(thread))
        panic("process: private worker could not be destroyed");
    record->execution = NULL;
    record->stack_region = NULL;
    record->tls_region = NULL;
    object_release(&record->object);
    if (__atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE))
        return MG_ERR_SHUTDOWN;
    return rollback_error;
}

void process_user_context_enter(void)
{
    kernel_thread_t *thread = thread_current();
    user_execution_context_t *context = thread ? &thread->user_context : NULL;

    if (!thread || !thread->process ||
        !__atomic_load_n(&context->valid, __ATOMIC_ACQUIRE))
        panic("process: invalid Ring 3 context");
    ring3_enter(context->instruction_pointer, context->stack_pointer,
                context->rdi, context->rsi, context->fs_base);
}

static void process_user_thread_entry(void *argument)
{
    process_t *process = (process_t *)argument;
    if (!process || process->state != PROCESS_STATE_ACTIVE ||
        !process->image_loaded)
        return;
    if (!thread_current() || thread_current()->process != process ||
        !__atomic_load_n(&thread_current()->user_context.valid,
                         __ATOMIC_ACQUIRE))
        return;
    process_user_context_enter();
}

static int process_spawn_args_with_context_internal(
    process_t *parent, const process_args_t *args,
    const process_credentials_t *credentials, mg_session_id_t session_id,
    bool session_shell, const char *initial_cwd, bool system_service,
    u32 service_id, bool inherit_output, process_handle_t output_handle,
    process_handle_t *out_handle)
{
    kernel_thread_t *thread;
    process_t *child;
    PROCESS_SPAWN_OBJECT_REFS_AUTO process_spawn_object_refs_t refs = {0};
    kernel_object_t *console;
    kernel_object_t *output;
    process_handle_t child_handle;
    process_handle_t child_input_handle;
    process_handle_t parent_handle;
    PROCESS_STRING_AUTO char *bin_path = NULL;
    char resolved_path[512];
    char process_name[32];
    const char *process_name_source;

    if (!parent || parent->state != PROCESS_STATE_ACTIVE || !args ||
        !out_handle || (credentials && !identity_credentials_valid(credentials)) ||
        (!credentials && parent->system_service) ||
        (system_service && (!credentials || !identity_credentials_is_system(credentials) ||
                            !service_id)) ||
        (initial_cwd && (initial_cwd[0] != '/' ||
                         strlen(initial_cwd) > MG_PATH_MAX_BYTES))) {
        return VFS_ERR_BAD_FORMAT;
    }

    int path_result = process_arguments_executable_path(args, &bin_path);
    if (path_result != VFS_OK) return path_result;

    VFS_LOCATION_AUTO vfs_location_t *executable = NULL;
    int resolution = process_lookup_path(parent, bin_path, &executable);
    if (resolution != VFS_OK) return resolution;
    resolution = vfs_location_path(executable, resolved_path, sizeof(resolved_path), NULL);
    if (resolution != VFS_OK && resolution != VFS_ERR_BUFFER_TOO_SMALL)
        return resolution;
    if (resolution == VFS_ERR_BUFFER_TOO_SMALL) resolved_path[0] = '\0';

    /* Console input and stdout are independent. A child can retain keyboard
     * input while writing to a file; interactive screen controls require its
     * caller to select console stdout explicitly when output is redirected. */
    console = process_handle_lookup(parent, PROCESS_INITIAL_STDIN_HANDLE,
                                    OBJECT_TYPE_CONSOLE,
                                    OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE);
    if (!console) {
        /* PID 1 predates the split handles and owns only its initial console
         * slot.  Keep it usable as the parent of system services. */
        console = process_handle_lookup(parent, PROCESS_INITIAL_CONSOLE_HANDLE,
                                        OBJECT_TYPE_CONSOLE,
                                        OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE);
    }
    if (!console) return VFS_ERR_BAD_FORMAT;
    refs.console = console;
    output = console;
    if (inherit_output) {
        output = process_handle_lookup_any(parent,
                                           PROCESS_INITIAL_CONSOLE_HANDLE,
                                           OBJECT_TYPE_INVALID,
                                           OBJECT_RIGHT_WRITE);
        if (!output || (output->type != OBJECT_TYPE_FILE &&
                        output->type != OBJECT_TYPE_CONSOLE)) {
            if (output) object_release(output);
            return VFS_ERR_BAD_FORMAT;
        }
        refs.separate_output = output;
    } else if (output_handle != 0) {
        output = process_handle_lookup_any(parent, output_handle,
                                           OBJECT_TYPE_INVALID,
                                           OBJECT_RIGHT_WRITE);
        if (!output || (output->type != OBJECT_TYPE_FILE &&
                        output->type != OBJECT_TYPE_CONSOLE)) {
            if (output) object_release(output);
            return VFS_ERR_BAD_FORMAT;
        }
        refs.separate_output = output;
    }

    process_name_source = bin_path;
    for (const char *cursor = bin_path; *cursor; cursor++) {
        if (*cursor == '/') process_name_source = cursor + 1;
    }
    if (!*process_name_source) return VFS_ERR_BAD_FORMAT;
    strncpy(process_name, process_name_source, sizeof(process_name) - 1U);
    process_name[sizeof(process_name) - 1U] = '\0';

    thread = thread_create_suspended("user", process_user_thread_entry, NULL);
    if (!thread) return VFS_ERR_NO_MEM;
    
    child = process_create(process_name, parent, thread);
    if (!child) {
        (void)thread_destroy(thread);
        return VFS_ERR_BAD_FORMAT;
    }
    thread->entry_argument = child;
    u64 child_flags = spin_lock_irqsave(&process_registry_lock);
    if (resolved_path[0]) {
        strncpy(child->executable_path, resolved_path,
                sizeof(child->executable_path) - 1U);
        child->executable_path[sizeof(child->executable_path) - 1U] = '\0';
    } else {
        /* Process snapshots expose a compact display field. Do not reject a
         * valid long executable path just because that diagnostic field is
         * smaller; the stable process name remains available to callers. */
        child->executable_path[0] = '\0';
    }

    if (credentials) {
        child->credentials = *credentials;
        child->credentials_initialized = true;
    }
    child->system_service = system_service;
    child->service_id = system_service ? service_id : 0;
    if (session_id != PROCESS_NO_SESSION) {
        child->session_id = session_id;
        child->session_shell = session_shell;
    }
    spin_unlock_irqrestore(&process_registry_lock, child_flags);
    if (session_id != PROCESS_NO_SESSION && initial_cwd &&
        process_chdir_result(child, initial_cwd) != VFS_OK) {
        process_abort(child);
        return VFS_ERR_BAD_FORMAT;
    }
    
    int load_result=elf_load_process_node_result(child,vfs_location_node(executable),
                                                 &child->entry_point,&child->user_stack_top);
    if (load_result != VFS_OK) {
        process_abort(child);
        return load_result;
    }

    int arguments_result = setup_user_stack_args(child, args);
    if (arguments_result != VFS_OK) {
        process_abort(child);
        return arguments_result;
    }
    if (!process_user_context_prepare_initial(child)) {
        process_abort(child);
        return VFS_ERR_BAD_FORMAT;
    }

    if (!process_handle_install(child, output, OBJECT_RIGHT_WRITE,
                                &child_handle) ||
        !process_handle_install(child, console,
                                OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE,
                                &child_input_handle)) {
        process_abort(child);
        return VFS_ERR_NO_MEM;
    }
    
    if (!process_handle_install(parent, &child->object, 0, &parent_handle)) {
        process_handle_close(child, child_handle);
        process_abort(child);
        return VFS_ERR_NO_MEM;
    }

    if (!scheduler_enqueue(thread)) {
        process_handle_close(parent, parent_handle);
        process_abort(child);
        return VFS_ERR_BAD_FORMAT;
    }
    if (!process_publish_launch(child))
        panic("process: failed to publish completed child launch");
    *out_handle = parent_handle;
    return VFS_OK;
}

int process_spawn_result(process_t *parent, const char *cmdline,
                           process_handle_t *out_handle)
{
    char bin_path[256];
    process_args_t args;
    if (!cmdline || !parse_spawn_cmdline(cmdline, bin_path, sizeof(bin_path), &args))
        return VFS_ERR_INVALID_PARAM;
    int result = process_spawn_args_with_context_internal(parent, &args, NULL,
        PROCESS_NO_SESSION, false, NULL, false, 0, false, 0, out_handle);
    process_arguments_destroy(&args);
    return result;
}

bool process_spawn(process_t *parent, const char *cmdline, process_handle_t *out_handle)
{
    return process_spawn_result(parent, cmdline, out_handle) == VFS_OK;
}

int process_spawn_arguments_result(process_t *parent,
    const process_arguments_t *args, bool inherit_output,
    process_handle_t output_handle, process_handle_t *out_handle)
{
    return process_spawn_args_with_context_internal(parent, args, NULL,
        PROCESS_NO_SESSION, false, NULL, false, 0, inherit_output,
        output_handle, out_handle);
}

static int process_spawn_kernel_argv_with_context(
    process_t *parent, const char *const *argv, u32 argc,
    const process_credentials_t *credentials, mg_session_id_t session_id,
    bool session_shell, const char *initial_cwd, bool system_service,
    u32 service_id, bool inherit_output, process_handle_t output_handle,
    process_handle_t *out_handle)
{
    process_args_t args;
    mg_result_t copied = process_arguments_collect(&args, (uintptr_t)argv, argc, NULL);
    if (copied != MG_OK)
        return copied == MG_ERR_NO_MEMORY ? VFS_ERR_NO_MEM : VFS_ERR_INVALID_PARAM;
    int result = process_spawn_args_with_context_internal(
        parent, &args, credentials, session_id, session_shell, initial_cwd,
        system_service, service_id, inherit_output, output_handle, out_handle);
    process_arguments_destroy(&args);
    return result;
}

static int process_spawn_kernel_argv(process_t *parent, const char *const *argv,
    u32 argc, bool inherit_output, process_handle_t output_handle,
    process_handle_t *out_handle)
{
    return process_spawn_kernel_argv_with_context(parent, argv, argc, NULL,
        PROCESS_NO_SESSION, false, NULL, false, 0, inherit_output,
        output_handle, out_handle);
}

int process_spawn_argv_result(process_t *parent, const char *const *argv,
                               u32 argc, process_handle_t *out_handle)
{
    return process_spawn_kernel_argv(parent, argv, argc, true, 0, out_handle);
}

int process_spawn_argv_with_output_result(process_t *parent,
    const char *const *argv, u32 argc, process_handle_t output_handle,
    process_handle_t *out_handle)
{
    if (!output_handle) return VFS_ERR_INVALID_PARAM;
    return process_spawn_kernel_argv(parent, argv, argc, false,
                                     output_handle, out_handle);
}

bool process_spawn_argv(process_t *parent, const char *const *argv,
                        u32 argc, process_handle_t *out_handle)
{
    return process_spawn_argv_result(parent, argv, argc, out_handle) == VFS_OK;
}

int process_spawn_with_output_result(process_t *parent, const char *cmdline,
                                       process_handle_t output_handle,
                                       process_handle_t *out_handle)
{
    char bin_path[256];
    process_args_t args;
    if (!cmdline || !parse_spawn_cmdline(cmdline, bin_path, sizeof(bin_path), &args))
        return VFS_ERR_INVALID_PARAM;
    int result = process_spawn_args_with_context_internal(parent, &args, NULL,
        PROCESS_NO_SESSION, false, NULL, false, 0, false, output_handle, out_handle);
    process_arguments_destroy(&args);
    return result;
}

bool process_spawn_with_output(process_t *parent, const char *cmdline,
                               process_handle_t output_handle,
                               process_handle_t *out_handle)
{
    return process_spawn_with_output_result(parent, cmdline, output_handle, out_handle) == VFS_OK;
}

bool process_redirect_output(process_t *process, process_handle_t output_handle,
                             process_handle_t *saved_handle)
{
    process_handle_slot_t *slots;
    kernel_object_t *output;
    kernel_object_t *current;
    kernel_object_t *old_output;
    process_handle_t saved;
    u64 flags;

    if (!process || process != process_current() ||
        process->state != PROCESS_STATE_ACTIVE || !saved_handle) return false;
    output = process_handle_lookup_reference(process, output_handle,
                                             OBJECT_TYPE_FILE,
                                             OBJECT_RIGHT_WRITE);
    current = process_handle_lookup_reference(
        process, PROCESS_INITIAL_CONSOLE_HANDLE, OBJECT_TYPE_CONSOLE,
        OBJECT_RIGHT_WRITE);
    if (!output || !current) {
        if (output) object_release(output);
        if (current) object_release(current);
        return false;
    }

    if (!process_handle_install(process, current, OBJECT_RIGHT_WRITE, &saved)) {
        object_release(output);
        object_release(current);
        return false;
    }

    if (!mutex_lock(&process->handle_growth_mutex)) {
        (void)process_handle_close(process, saved);
        object_release(output);
        object_release(current);
        return false;
    }
    flags = spin_lock_irqsave(&process->handle_lock);
    slots = handle_slots(process);
    if (process->output_redirected || !slots[0].active ||
        !slots[0].object) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        (void)process_handle_close(process, saved);
        object_release(output);
        object_release(current);
        return false;
    }
    old_output = slots[0].object;
    slots[0].object = output;
    slots[0].rights = OBJECT_RIGHT_WRITE;
    process_handle_revision_advance(process);
    process->output_redirected = true;
    process->redirected_output_saved_handle = saved;
    *saved_handle = saved;
    spin_unlock_irqrestore(&process->handle_lock, flags);
    mutex_unlock(&process->handle_growth_mutex);
    object_release(old_output);
    object_release(current);
    return true;
}

bool process_restore_output(process_t *process, process_handle_t saved_handle)
{
    process_handle_slot_t *slots;
    kernel_object_t *saved;
    kernel_object_t *saved_handle_object;
    kernel_object_t *old_output;
    u32 saved_index;
    u16 saved_generation;
    u64 flags;

    if (!process || process != process_current() ||
        process->state != PROCESS_STATE_ACTIVE ||
        !decode_handle(saved_handle, &saved_index, &saved_generation)) return false;
    saved = process_handle_lookup_reference(process, saved_handle,
                                            OBJECT_TYPE_CONSOLE,
                                            OBJECT_RIGHT_WRITE);
    if (!saved) return false;
    if (!mutex_lock(&process->handle_growth_mutex)) {
        object_release(saved);
        return false;
    }
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!process->handle_table || saved_index >= process->handle_capacity) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        object_release(saved);
        return false;
    }
    slots = handle_slots(process);
    if (!process->output_redirected ||
        saved_handle != process->redirected_output_saved_handle ||
        !slots[0].active || slots[0].generation != 1U ||
        !slots[saved_index].active ||
        slots[saved_index].generation != saved_generation ||
        slots[saved_index].object != saved) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        object_release(saved);
        return false;
    }
    saved_handle_object = slots[saved_index].object;
    slots[saved_index].object = NULL;
    slots[saved_index].rights = 0;
    slots[saved_index].active = false;
    old_output = slots[0].object;
    slots[0].object = saved;
    slots[0].rights = OBJECT_RIGHT_WRITE;
    process->output_redirected = false;
    process->redirected_output_saved_handle = 0;
    process_handle_revision_advance(process);
    spin_unlock_irqrestore(&process->handle_lock, flags);
    mutex_unlock(&process->handle_growth_mutex);
    object_release(old_output);
    object_release(saved_handle_object);
    return true;
}

bool process_spawn_argv_with_context(process_t *parent,
                                const char *const *argv, u32 argc,
                                const process_credentials_t *credentials,
                                mg_session_id_t session_id,
                                bool session_shell, const char *initial_cwd,
                                process_handle_t *out_handle)
{
    return process_spawn_kernel_argv_with_context(parent, argv, argc,
        credentials, session_id, session_shell, initial_cwd, false, 0,
        false, 0, out_handle) == VFS_OK;
}

int process_spawn_service_result(process_t *parent, u32 service_id,
                                 process_handle_t *out_handle)
{
    const kernel_service_definition_t *definition;
    kernel_object_t *object;
    process_credentials_t credentials;

    if (!parent || parent->pid != 1U || parent->state != PROCESS_STATE_ACTIVE ||
        !parent->system_service || parent->service_id != MG_SERVICE_SPROUT ||
        service_id == MG_SERVICE_SPROUT ||
        !parent->credentials_initialized ||
        !identity_credentials_valid(&parent->credentials) ||
        !service_definition_lookup(service_id, &definition) ||
        !identity_credentials_is_system(&parent->credentials)) {
        return MG_ERR_PRIVILEGE_REQUIRED;
    }
    credentials = identity_system_credentials();
    credentials.service_privileges = definition->privileges;
    const char *service_argv[] = { definition->path };
    if (process_spawn_kernel_argv_with_context(
            parent, service_argv, 1U, &credentials, PROCESS_NO_SESSION, false,
            NULL, true, service_id, false, 0, out_handle) != VFS_OK) {
        return MG_ERR_INVALID_EXEC;
    }
    object = process_handle_lookup_any(parent, *out_handle,
                                       OBJECT_TYPE_PROCESS, 0);
    if (!object) {
        (void)process_handle_close(parent, *out_handle);
        return MG_ERR_IO;
    }
    object_release(object);
    return MG_OK;
}

bool process_spawn_service(process_t *parent, u32 service_id,
                           process_handle_t *out_handle)
{
    return process_spawn_service_result(parent, service_id, out_handle) ==
           MG_OK;
}

u64 process_handle_pid(process_t *process, process_handle_t handle)
{
    kernel_object_t *object;
    process_t *child;

    object = process_handle_lookup_any(process, handle, OBJECT_TYPE_PROCESS, 0);
    if (!object) return 0;
    child = object_process(object);
    u64 pid = child->pid;
    process_handle_reference_release(object);
    return pid;
}

int process_poll(process_t *parent, process_handle_t handle, i32 *out_status)
{
    kernel_object_t *object;
    process_t *child;

    if (!parent || parent->state != PROCESS_STATE_ACTIVE || !out_status)
        return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup_any(parent, handle, OBJECT_TYPE_PROCESS, 0);
    if (!object) return MG_ERR_INVALID_HANDLE;
    child = object_process(object);
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    if (child == parent || child->parent != parent || child->wait_collected) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        process_handle_reference_release(object);
        return MG_ERR_NOT_CHILD;
    }
    if (child->state != PROCESS_STATE_TERMINATED) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        process_handle_reference_release(object);
        return MG_ERR_WOULD_BLOCK;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (!process_finalize_exit(child)) {
        process_handle_reference_release(object);
        return MG_ERR_WOULD_BLOCK;
    }
    flags = spin_lock_irqsave(&process_registry_lock);
    if (child->parent != parent || child->wait_collected ||
        child->state != PROCESS_STATE_TERMINATED) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        process_handle_reference_release(object);
        return MG_ERR_NOT_CHILD;
    }
    *out_status = child->exit_status;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    process_handle_reference_release(object);
    return MG_OK;
}

int process_terminate_child(process_t *parent, process_handle_t handle,
                            i32 status)
{
    kernel_object_t *object;
    process_t *child;

    if (!parent || parent->state != PROCESS_STATE_ACTIVE)
        return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup_any(parent, handle, OBJECT_TYPE_PROCESS, 0);
    if (!object) return MG_ERR_INVALID_HANDLE;
    child = object_process(object);
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    if (child == parent || child->parent != parent ||
        child->state != PROCESS_STATE_ACTIVE) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        process_handle_reference_release(object);
        return MG_ERR_NOT_CHILD;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    bool terminated = process_terminate_external(child, status);
    process_handle_reference_release(object);
    return terminated ? MG_OK : MG_ERR_BUSY;
}

int process_wait(process_t *parent, process_handle_t handle,
                 i32 *out_status)
{
    kernel_object_t *object;
    process_t *child;
    kernel_thread_t *thread;
    u64 flags;
    bool waiter_reader_pinned = false;
    bool registry_lock_irqsave = false;
    bool interrupted;

    if (!parent || parent->state != PROCESS_STATE_ACTIVE || !out_status) {
        return MG_ERR_BAD_ARGUMENT;
    }
    object = process_handle_lookup_any(parent, handle, OBJECT_TYPE_PROCESS, 0);
    if (!object) return MG_ERR_INVALID_HANDLE;
    child = object_process(object);
    u64 wait_flags = process_cli_save();
    spin_lock(&process_registry_lock);
    if (parent->state != PROCESS_STATE_ACTIVE || parent->exit_requested ||
        parent->termination_pending || parent->shutdown_requested) {
        spin_unlock(&process_registry_lock);
        process_cli_restore(wait_flags);
        process_handle_reference_release(object);
        return MG_ERR_SHUTDOWN;
    }
    if (child == parent || child->parent != parent || child->wait_collected) {
        spin_unlock(&process_registry_lock);
        process_cli_restore(wait_flags);
        process_handle_reference_release(object);
        return MG_ERR_INVALID_HANDLE;
    }
    if (child->state != PROCESS_STATE_TERMINATED) {
        thread = thread_current();
        if (parent->waiting_child || !thread ||
            thread->process != parent ||
            thread->process_state != THREAD_PROCESS_ATTACHED ||
            child->wait_claimed) {
            spin_unlock(&process_registry_lock);
            process_cli_restore(wait_flags);
            process_handle_reference_release(object);
            return MG_ERR_BUSY;
        }
        if (!process_thread_list_read_acquire_locked(parent)) {
            spin_unlock(&process_registry_lock);
            process_cli_restore(wait_flags);
            process_handle_reference_release(object);
            return MG_ERR_BUSY;
        }
        waiter_reader_pinned = true;
        scheduler_clear_event_wake(thread);
        parent->waiting_child = child;
        parent->waiting_thread = thread;

        /* Keep the registration and reader pin until the child predicate is
         * true or this process is interrupted. Completion uses the scheduler
         * event latch, so it is safe whether it runs before scheduler_block()
         * parks this thread or after the thread is already BLOCKED. Unrelated
         * event wakes simply recheck the predicate and wait again. */
        for (;;) {
            spin_unlock(&process_registry_lock);
            bool woke = scheduler_block();
            spin_lock(&process_registry_lock);

            interrupted = parent->state != PROCESS_STATE_ACTIVE ||
                parent->exit_requested || parent->termination_pending ||
                parent->shutdown_requested;
            if (interrupted || child->state == PROCESS_STATE_TERMINATED)
                break;
            if (!woke) {
                if (parent->waiting_thread == thread &&
                    parent->waiting_child == child) {
                    scheduler_clear_event_wake(thread);
                    parent->waiting_child = NULL;
                    parent->waiting_thread = NULL;
                }
                spin_unlock(&process_registry_lock);
                process_cli_restore(wait_flags);
                if (waiter_reader_pinned) {
                    process_thread_list_read_release(parent);
                    waiter_reader_pinned = false;
                }
                process_handle_reference_release(object);
                return interrupted ? MG_ERR_SHUTDOWN : MG_ERR_BUSY;
            }
            /* A wake for another event is not a child-completion predicate.
             * Keep the registration intact and block again. */
        }
        if (interrupted) {
            if (parent->waiting_thread == thread &&
                parent->waiting_child == child) {
                scheduler_clear_event_wake(thread);
                parent->waiting_child = NULL;
                parent->waiting_thread = NULL;
            }
            spin_unlock(&process_registry_lock);
            process_cli_restore(wait_flags);
            if (waiter_reader_pinned) {
                process_thread_list_read_release(parent);
                waiter_reader_pinned = false;
            }
            process_handle_reference_release(object);
            return MG_ERR_SHUTDOWN;
        }
        if (parent->waiting_thread == thread &&
            parent->waiting_child == child) {
            parent->waiting_child = NULL;
            parent->waiting_thread = NULL;
        }
        scheduler_clear_event_wake(thread);
        spin_unlock(&process_registry_lock);
        process_cli_restore(wait_flags);
        if (waiter_reader_pinned) {
            process_thread_list_read_release(parent);
            waiter_reader_pinned = false;
        }
        /* Reader release may run cleanup follow-up, so it runs with the
         * caller's interrupt state restored and no registry lock held. Reacquire
         * the registry with IRQ-save semantics: timer/keyboard interrupt paths
         * also take this lock, and a plain spin_lock here would permit a local
         * interrupt to deadlock against a remote lock holder. */
        flags = spin_lock_irqsave(&process_registry_lock);
        registry_lock_irqsave = true;
        if (child->state != PROCESS_STATE_TERMINATED) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            process_handle_reference_release(object);
            return process_shutdown_requested(parent) ? MG_ERR_SHUTDOWN :
                MG_ERR_BUSY;
        }
    }
    if (child->wait_claimed) {
        if (registry_lock_irqsave)
            spin_unlock_irqrestore(&process_registry_lock, flags);
        else {
            spin_unlock(&process_registry_lock);
            process_cli_restore(wait_flags);
        }
        process_handle_reference_release(object);
        return MG_ERR_BUSY;
    }
    child->wait_claimed = true;
    if (registry_lock_irqsave)
        spin_unlock_irqrestore(&process_registry_lock, flags);
    else {
        spin_unlock(&process_registry_lock);
        process_cli_restore(wait_flags);
    }

    if (!process_finalize_exit(child)) {
        flags = spin_lock_irqsave(&process_registry_lock);
        child->wait_claimed = false;
        spin_unlock_irqrestore(&process_registry_lock, flags);
        process_handle_reference_release(object);
        return MG_ERR_BUSY;
    }

    flags = spin_lock_irqsave(&process_registry_lock);
    if (child->parent != parent || child->wait_collected ||
        child->state != PROCESS_STATE_TERMINATED || !child->wait_claimed) {
        child->wait_claimed = false;
        spin_unlock_irqrestore(&process_registry_lock, flags);
        process_handle_reference_release(object);
        return MG_ERR_INVALID_HANDLE;
    }
    *out_status = child->exit_status;
    child->wait_collected = true;
    child->wait_claimed = false;
    process_unlink_locked(child);
    child->owner_reference_held = false;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    object_release(&child->object); /* drop the process's owner reference */
    process_handle_reference_release(object);
    return MG_OK;
}

bool process_handle_install(process_t *process, kernel_object_t *object,
                            u32 rights, process_handle_t *out_handle)
{
    if (!process || process->state != PROCESS_STATE_ACTIVE || !object ||
        !__atomic_load_n(&object->ref_count, __ATOMIC_ACQUIRE) || !out_handle ||
        (rights & ~(OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE))) return false;
    for (;;) {
        if (!mutex_lock(&process->handle_growth_mutex)) return false;
        u64 flags = spin_lock_irqsave(&process->handle_lock);
        process_handle_slot_t *slots = handle_slots(process);
        usize capacity = process->handle_capacity;
        usize first = capacity ? process->next_handle_slot % capacity : 0;
        for (usize offset = 0; offset < capacity; offset++) {
            usize i = (first + offset) % capacity;
            if (!slots[i].active && !slots[i].reserved) {
                u16 generation = slots[i].generation + 1U;
                if (!generation) generation = 1;
                if (!object_reference(object)) {
                    spin_unlock_irqrestore(&process->handle_lock, flags);
                    mutex_unlock(&process->handle_growth_mutex);
                    return false;
                }
                slots[i].object = object;
                slots[i].rights = rights;
                slots[i].generation = generation;
                slots[i].active = true;
                process->next_handle_slot = (i + 1U) % capacity;
                process_handle_revision_advance(process);
                *out_handle = encode_handle((u32)i, generation);
                spin_unlock_irqrestore(&process->handle_lock, flags);
                mutex_unlock(&process->handle_growth_mutex);
                return true;
            }
        }
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        if (!process_handle_grow(process)) return false;
    }
}

bool process_handle_reserve(process_t *process, kernel_object_t *object,
                            u32 rights, process_handle_t *out_handle)
{
    if (!process || process->state != PROCESS_STATE_ACTIVE ||
        !process->handle_table || !object ||
        !__atomic_load_n(&object->ref_count, __ATOMIC_ACQUIRE) ||
        !out_handle || (rights & ~(OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE)))
        return false;
    for (;;) {
        if (!mutex_lock(&process->handle_growth_mutex)) return false;
        u64 flags = spin_lock_irqsave(&process->handle_lock);
        process_handle_slot_t *slots = handle_slots(process);
        usize capacity = process->handle_capacity;
        usize first = capacity ? process->next_handle_slot % capacity : 0;
        for (usize offset = 0; offset < capacity; offset++) {
            usize i = (first + offset) % capacity;
            if (slots[i].active || slots[i].reserved) continue;
            u16 generation = slots[i].generation + 1U;
            if (!generation) generation = 1;
            slots[i].object = object; /* Transfer the caller's reference. */
            slots[i].rights = rights;
            slots[i].generation = generation;
            slots[i].reserved = true;
            process->next_handle_slot = (i + 1U) % capacity;
            process_handle_revision_advance(process);
            *out_handle = encode_handle((u32)i, generation);
            spin_unlock_irqrestore(&process->handle_lock, flags);
            mutex_unlock(&process->handle_growth_mutex);
            return true;
        }
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        if (!process_handle_grow(process)) return false;
    }
}

bool process_handle_activate(process_t *process, process_handle_t handle)
{
    u32 index;
    u16 generation;
    u64 flags;
    process_handle_slot_t *slot;

    if (!process || !decode_handle(handle, &index, &generation)) return false;
    if (!mutex_lock(&process->handle_growth_mutex)) return false;
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!process->handle_table || index >= process->handle_capacity) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        return false;
    }
    slot = &handle_slots(process)[index];
    if (process->state != PROCESS_STATE_ACTIVE || !slot->reserved ||
        slot->active || slot->generation != generation || !slot->object) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        return false;
    }
    slot->active = true;
    slot->reserved = false;
    process_handle_revision_advance(process);
    spin_unlock_irqrestore(&process->handle_lock, flags);
    mutex_unlock(&process->handle_growth_mutex);
    return true;
}

bool process_handle_cancel(process_t *process, process_handle_t handle)
{
    u32 index;
    u16 generation;
    u64 flags;
    kernel_object_t *object;
    process_handle_slot_t *slot;

    if (!process || !decode_handle(handle, &index, &generation)) return false;
    if (!mutex_lock(&process->handle_growth_mutex)) return false;
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!process->handle_table || index >= process->handle_capacity) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        return false;
    }
    slot = &handle_slots(process)[index];
    if (!slot->reserved || slot->active || slot->generation != generation) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        return false;
    }
    object = slot->object;
    slot->object = NULL;
    slot->rights = 0;
    slot->reserved = false;
    if (index < process->next_handle_slot) process->next_handle_slot = index;
    process_handle_revision_advance(process);
    spin_unlock_irqrestore(&process->handle_lock, flags);
    mutex_unlock(&process->handle_growth_mutex);
    if (object) object_release(object);
    return true;
}

kernel_object_t *process_handle_lookup_any(process_t *process,
                                           process_handle_t handle,
                                           kernel_object_type_t type,
                                           u32 required_rights)
{
    return process_handle_lookup_reference(process, handle, type,
                                           required_rights);
}

static kernel_object_t *process_handle_lookup_reference(
    process_t *process, process_handle_t handle,
    kernel_object_type_t type, u32 required_rights)
{
    u32 index;
    u16 generation;
    kernel_object_t *object;
    u64 flags;

    if (!process || !decode_handle(handle, &index, &generation)) return NULL;
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!process->handle_table || index >= process->handle_capacity) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        return NULL;
    }
    process_handle_slot_t *slots = handle_slots(process);
    if (!slots[index].active || slots[index].generation != generation ||
        !slots[index].object ||
        (type != OBJECT_TYPE_INVALID && slots[index].object->type != type) ||
        (slots[index].rights & required_rights) != required_rights ||
        !object_reference(slots[index].object)) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        return NULL;
    }
    object = slots[index].object;
    spin_unlock_irqrestore(&process->handle_lock, flags);
    return object;
}

kernel_object_t *process_handle_lookup(process_t *process,
                                       process_handle_t handle,
                                       kernel_object_type_t type,
                                       u32 required_rights)
{
    if (!process || process->state != PROCESS_STATE_ACTIVE) return NULL;
    return process_handle_lookup_reference(process, handle, type,
                                           required_rights);
}

static process_thread_record_t *process_thread_record_from_object(
    kernel_object_t *object)
{
    if (!object || object->type != OBJECT_TYPE_THREAD) return NULL;
    return (process_thread_record_t *)((u8 *)object -
        __builtin_offsetof(process_thread_record_t, object));
}

bool process_thread_handle_close_allowed(kernel_object_t *object)
{
    process_thread_record_t *record =
        process_thread_record_from_object(object);
    bool allowed;
    u64 flags;

    if (!record) return false;
    flags = spin_lock_irqsave(&process_registry_lock);
    allowed = record->joined || record->detached;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return allowed;
}

int process_thread_join(process_t *process, process_handle_t handle,
                        u64 *out_result)
{
    kernel_object_t *object;
    process_thread_record_t *record;
    kernel_thread_t *thread = thread_current();
    u64 wait_flags;
    u64 result = 0;
    int failure = MG_OK;
    bool claimed = false;

    if (!process || !out_result || !thread || thread->process != process)
        return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup(process, handle, OBJECT_TYPE_THREAD, 0);
    if (!object) return MG_ERR_INVALID_HANDLE;
    record = process_thread_record_from_object(object);
    if (!record) {
        object_release(object);
        return MG_ERR_INVALID_HANDLE;
    }

    wait_flags = process_cli_save();
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    if (!record->listed || record->process != process) {
        failure = MG_ERR_INVALID_HANDLE;
    } else if (record->id == thread->id) {
        failure = MG_ERR_BAD_ARGUMENT;
    } else if (record->joined || record->detached ||
               record->join_claimed) {
        failure = MG_ERR_BUSY;
    } else if (process->state != PROCESS_STATE_ACTIVE ||
               __atomic_load_n(&process->exit_requested, __ATOMIC_ACQUIRE)) {
        failure = MG_ERR_SHUTDOWN;
    } else {
        record->join_claimed = true;
        record->join_waiter = thread;
        claimed = true;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (failure != MG_OK) {
        process_cli_restore(wait_flags);
        object_release(object);
        return failure;
    }

    for (;;) {
        flags = spin_lock_irqsave(&process_registry_lock);
        if (!record->listed || record->process != process) {
            failure = MG_ERR_INVALID_HANDLE;
        } else if (process->state != PROCESS_STATE_ACTIVE ||
                   __atomic_load_n(&process->exit_requested,
                                   __ATOMIC_ACQUIRE) ||
                   process_thread_exit_requested(thread) ||
                   process_shutdown_requested(process)) {
            failure = MG_ERR_SHUTDOWN;
        } else if (record->completed) {
            result = record->result;
            record->joined = true;
            __atomic_store_n(&process->worker_reap_pending, true,
                             __ATOMIC_RELEASE);
            record->join_claimed = false;
            record->join_waiter = NULL;
            claimed = false;
        }
        if (failure != MG_OK || !claimed) {
            if (failure != MG_OK && record->join_claimed &&
                record->join_waiter == thread) {
                record->join_claimed = false;
                record->join_waiter = NULL;
                claimed = false;
            }
            spin_unlock_irqrestore(&process_registry_lock, flags);
            break;
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);

        if (process_take_interrupt(process)) {
            flags = spin_lock_irqsave(&process_registry_lock);
            if (record->join_claimed && record->join_waiter == thread) {
                record->join_claimed = false;
                record->join_waiter = NULL;
            }
            spin_unlock_irqrestore(&process_registry_lock, flags);
            claimed = false;
            failure = process_shutdown_requested(process)
                ? MG_ERR_SHUTDOWN : MG_ERR_INTERRUPTED;
            break;
        }
        if (!scheduler_block()) {
            flags = spin_lock_irqsave(&process_registry_lock);
            if (record->join_claimed && record->join_waiter == thread) {
                record->join_claimed = false;
                record->join_waiter = NULL;
            }
            spin_unlock_irqrestore(&process_registry_lock, flags);
            claimed = false;
            failure = MG_ERR_BUSY;
            break;
        }
    }
    scheduler_clear_event_wake(thread);
    process_cli_restore(wait_flags);

    if (failure == MG_OK && !claimed) {
        process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);
        *out_result = result;
        /* joined is committed before the slot is consumed. A concurrent
         * explicit close may win the slot race; it cannot undo the join. */
        (void)process_handle_close(process, handle);
    }
    object_release(object);
    return failure;
}

int process_thread_detach(process_t *process, process_handle_t handle)
{
    kernel_object_t *object;
    process_thread_record_t *record;
    u64 flags;

    if (!process) return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup(process, handle, OBJECT_TYPE_THREAD, 0);
    if (!object) return MG_ERR_INVALID_HANDLE;
    record = process_thread_record_from_object(object);
    if (!record) {
        object_release(object);
        return MG_ERR_INVALID_HANDLE;
    }

    flags = spin_lock_irqsave(&process_registry_lock);
    int result = MG_OK;
    if (!record->listed || record->process != process)
        result = MG_ERR_INVALID_HANDLE;
    else if (record->joined || record->detached || record->join_claimed)
        result = MG_ERR_BUSY;
    else {
        record->detached = true;
        __atomic_store_n(&process->worker_reap_pending, true,
                         __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);

    if (result == MG_OK) {
        /* Detach, unlike ordinary handle close, is the explicit operation
         * that releases join entitlement. A competing close may consume the
         * already-detached handle first. */
        (void)process_handle_close(process, handle);
        process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);
    }
    object_release(object);
    return result;
}

bool process_handle_close(process_t *process, process_handle_t handle)
{
    u32 index;
    u16 generation;
    kernel_object_t *object;
    u64 flags;
    if (!process || !decode_handle(handle, &index, &generation)) return false;

    /* A raw close cannot release a live worker's join entitlement. Retain
     * the object while checking policy outside handle_lock; lifecycle state
     * only moves toward joined/detached, so a permitted close remains
     * permitted when the slot is revalidated below. Internal process-wide
     * cleanup uses process_handle_close_all() and deliberately bypasses this
     * user-operation rule. */
    object = process_handle_lookup_reference(process, handle,
        OBJECT_TYPE_INVALID, 0);
    if (!object) return false;
    bool allowed = object->type != OBJECT_TYPE_THREAD ||
        process_thread_handle_close_allowed(object);
    if (!allowed) {
        process_handle_reference_release(object);
        return false;
    }

    if (!mutex_lock(&process->handle_growth_mutex)) {
        process_handle_reference_release(object);
        return false;
    }
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!process->handle_table || index >= process->handle_capacity) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        process_handle_reference_release(object);
        return false;
    }
    process_handle_slot_t *slots = handle_slots(process);
    if (!slots[index].active || slots[index].generation != generation) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        mutex_unlock(&process->handle_growth_mutex);
        process_handle_reference_release(object);
        return false;
    }
    object = slots[index].object;
    slots[index].object = NULL;
    slots[index].rights = 0;
    slots[index].active = false;
    if (index < process->next_handle_slot) process->next_handle_slot = index;
    process_handle_revision_advance(process);
    spin_unlock_irqrestore(&process->handle_lock, flags);
    mutex_unlock(&process->handle_growth_mutex);
    object_release(object);
    process_handle_reference_release(object);
    return true;
}

void process_handle_close_all(process_t *process)
{
    usize capacity;
    if (!process || !process->handle_table) return;
    if (!mutex_lock(&process->handle_growth_mutex)) return;
    capacity = process->handle_capacity;
    process->next_handle_slot = 0;
    for (usize i = 0; i < capacity; i++) {
        kernel_object_t *object = NULL;
        u64 flags = spin_lock_irqsave(&process->handle_lock);
        process_handle_slot_t *slots = handle_slots(process);
        if (slots[i].active || slots[i].reserved) {
            object = slots[i].object;
            slots[i].object = NULL;
            slots[i].rights = 0;
            slots[i].active = false;
            slots[i].reserved = false;
            process_handle_revision_advance(process);
        }
        spin_unlock_irqrestore(&process->handle_lock, flags);
        if (object) object_release(object);
    }
    mutex_unlock(&process->handle_growth_mutex);
}

const char *process_state_name(process_state_t state)
{
    switch (state) {
        case PROCESS_STATE_ACTIVE: return "active";
        case PROCESS_STATE_EXITING: return "exiting";
        case PROCESS_STATE_TERMINATED: return "terminated";
        default: return "unknown";
    }
}

static const char *process_role_name(mg_identity_role_t role)
{
    if (role == MG_IDENTITY_ROLE_REGULAR) return "regular";
    if (role == MG_IDENTITY_ROLE_ADMIN) return "admin";
    if (role == MG_IDENTITY_ROLE_SYSTEM) return "system";
    return "unknown";
}

void process_dump(void)
{
    process_t *process;
    u64 flags = spin_lock_irqsave(&process_registry_lock);

    kprint("Processes:\n");
    for (process = all_processes; process; process = process->next_all) {
        if (!process->launch_ready) continue;
        process_credentials_t credentials = process->credentials;
        if (process->state == PROCESS_STATE_ACTIVE &&
            process->credentials_initialized) {
            (void)identity_credentials_effective(&process->credentials,
                                                  &credentials);
        }
        kprint("  pid=%llu name=%s state=%s uid=%u role=%s session=%llu"
               " shell=%s service=%s(%u) privileges=0x%x\n",
               process->pid, process->name, process_state_name(process->state),
               credentials.uid, process_role_name(credentials.role),
               process->session_id, process->session_shell ? "yes" : "no",
               process->system_service ? "yes" : "no", process->service_id,
               credentials.service_privileges);
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
}

static u32 process_inspection_state(const process_t *process)
{
    if (!process || process->state == PROCESS_STATE_TERMINATED)
        return MG_PROCESS_INSPECTION_EXITED;
    bool ready = false;
    for (kernel_thread_t *thread = process->threads; thread;
         thread = thread->process_next) {
        thread_state_t state = scheduler_thread_state_snapshot(thread);
        if (state == THREAD_STATE_RUNNING) return MG_PROCESS_INSPECTION_RUNNING;
        if (state == THREAD_STATE_READY) ready = true;
    }
    return ready ? MG_PROCESS_INSPECTION_READY : MG_PROCESS_INSPECTION_BLOCKED;
}

/* Caller holds process_registry_lock. These copied flags help distinguish a
 * transient worker/mapping cleanup from a stable process memory snapshot. */
static u32 process_snapshot_reap_flags_locked(const process_t *process)
{
    u32 flags = 0;
    if (!process) return 0;
    if (__atomic_load_n(&process->worker_reap_pending, __ATOMIC_ACQUIRE))
        flags |= MG_PROCESS_FLAG_WORKER_REAP_PENDING;
    if (__atomic_load_n(&process->user_region_reap_pending,
                        __ATOMIC_ACQUIRE))
        flags |= MG_PROCESS_FLAG_USER_REGION_REAP_PENDING;
    if (process->worker_reapers)
        flags |= MG_PROCESS_FLAG_WORKER_REAPER_ACTIVE;
    return flags;
}

u32 process_snapshot_read(u32 offset, mg_process_info_t *output,
                          u32 capacity, u32 *out_total)
{
    process_t *process;
    u32 total = 0;
    u32 copied = 0;

    if (!output || !capacity || capacity > MG_PROCESS_SNAPSHOT_PAGE_MAX ||
        !out_total) return 0;
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    for (process = all_processes; process; process = process->next_all) {
        if (!process->launch_ready) continue;
        process_credentials_t credentials = process->credentials;
        mg_process_info_t info;
        user_identity_t account;
        const kernel_service_definition_t *definition = NULL;

        if (total >= offset && copied < capacity) {
            memset(&info, 0, sizeof(info));
            info.pid = process->pid;
            info.parent_pid = process->parent ? process->parent->pid : 0;
            info.session_id = process->session_id;
            info.state = process_inspection_state(process);
            info.running_cpu = scheduler_process_running_cpu_snapshot(
                process->threads);
            info.flags = (process->system_service
                          ? MG_PROCESS_FLAG_SYSTEM_SERVICE : 0U) |
                         (process->session_shell
                          ? MG_PROCESS_FLAG_SESSION_SHELL : 0U) |
                         process_snapshot_reap_flags_locked(process);
            if (process->credentials_initialized &&
                identity_credentials_effective(&process->credentials,
                                               &credentials)) {
                /* Use live human role data, while preserving explicit
                 * service capabilities for system processes. */
            }
            info.uid = credentials.uid;
            if (credentials.uid == MG_UID_SYSTEM) {
                info.role = MG_INSPECTION_ROLE_SYSTEM;
                strncpy(info.username, "system", sizeof(info.username) - 1U);
            } else {
                info.role = credentials.role;
                if (identity_registry_lookup_uid(credentials.uid, &account))
                    strncpy(info.username, account.username,
                            sizeof(info.username) - 1U);
            }
            strncpy(info.name, process->name, sizeof(info.name) - 1U);
            info.cpu_time_ms = __atomic_load_n(&process->cpu_time_ticks,
                                               __ATOMIC_RELAXED);
            info.memory_bytes = vmm_address_space_user_memory_bytes(
                process->address_space);
            strncpy(info.executable_path, process->executable_path,
                    sizeof(info.executable_path) - 1U);
            if (process->system_service &&
                service_definition_lookup(process->service_id, &definition) &&
                definition && definition->name) {
                strncpy(info.service, definition->name,
                        sizeof(info.service) - 1U);
                if (!info.executable_path[0] && definition->path) {
                    strncpy(info.executable_path, definition->path,
                            sizeof(info.executable_path) - 1U);
                }
            }
            output[copied++] = info;
        }
        total++;
    }
    *out_total = total;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return copied;
}
