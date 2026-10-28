#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production process handle-table implementation."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "kernel/src/process.c").read_text()


def function(name):
    for match in re.finditer(
            r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\(",
            SOURCE, re.M):
        brace = SOURCE.index("{", match.end())
        if ";" in SOURCE[match.end():brace]:
            continue
        depth = 1
        end = brace + 1
        while depth:
            depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
            end += 1
        return SOURCE[match.start():end] + "\n"
    raise ValueError(name)


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef int32_t i32;
typedef size_t usize;
typedef void kernel_thread_t;
typedef uint32_t process_handle_t;
#define PROCESS_HANDLE_SLOTS_INITIAL 16U
#define PROCESS_HANDLE_SLOTS_MAX 65535U
#define HANDLE_INDEX_MASK 0xffffU
typedef enum { OBJECT_TYPE_INVALID, OBJECT_TYPE_TEST,
               OBJECT_TYPE_PROCESS } kernel_object_type_t;
enum { PROCESS_STATE_ACTIVE = 1, PROCESS_STATE_TERMINATED = 2,
       OBJECT_RIGHT_READ = 1, OBJECT_RIGHT_WRITE = 2 };
#define MG_ERR_BAD_ARGUMENT -1
#define MG_ERR_INVALID_HANDLE -2
#define MG_ERR_NOT_CHILD -3
#define MG_ERR_WOULD_BLOCK -4
#define MG_ERR_BUSY -5
#define MG_ERR_SHUTDOWN -6
#define MG_OK 0
typedef pthread_mutex_t spinlock_t;
typedef struct { pthread_mutex_t native; volatile unsigned waiters;
                 bool track_waiters; } mutex_t;
typedef struct kernel_object {
    kernel_object_type_t type;
    u32 ref_count;
} kernel_object_t;
typedef struct process {
    int state;
    u32 active_syscalls;
    bool termination_pending, launch_ready;
    void *main_thread;
    struct process *parent;
    bool wait_collected;
    bool owner_reference_held;
    i32 exit_status;
    struct process *waiting_child;
    void *waiting_thread;
    kernel_object_t object;
    void *handle_table;
    usize handle_capacity;
    spinlock_t handle_lock;
    mutex_t handle_growth_mutex;
    u64 handle_revision;
    usize next_handle_slot;
} process_t;
typedef struct {
    kernel_object_t *object;
    u32 rights;
    uint16_t generation;
    bool active;
    bool reserved;
} process_handle_slot_t;
static long allocation_countdown = -1;
static _Atomic unsigned allocations, frees;
static _Thread_local unsigned spin_depth;
static spinlock_t process_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static bool pause_growth_allocation;
static pthread_barrier_t *growth_allocation_entered, *growth_allocation_release;
static void *current_thread;
static bool block_result;
static bool sleep_result;
static bool thread_exit_complete;
static bool shutdown_requested;
static u64 spin_lock_irqsave(spinlock_t *lock)
{ assert(pthread_mutex_lock(lock) == 0); spin_depth++; return 0; }
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{ (void)flags; assert(spin_depth); spin_depth--; assert(pthread_mutex_unlock(lock) == 0); }
static bool mutex_lock(mutex_t *mutex)
{ __atomic_add_fetch(&mutex->waiters, 1U, __ATOMIC_RELEASE); int r=pthread_mutex_lock(&mutex->native); __atomic_sub_fetch(&mutex->waiters,1U,__ATOMIC_RELEASE); return r==0; }
static bool mutex_unlock(mutex_t *mutex)
{ return pthread_mutex_unlock(&mutex->native)==0; }
static void *kmalloc(usize size)
{
    assert(spin_depth == 0);
    if (pause_growth_allocation) {
        pause_growth_allocation = false;
        int r=pthread_barrier_wait(growth_allocation_entered);
        assert(r==0||r==PTHREAD_BARRIER_SERIAL_THREAD);
        r=pthread_barrier_wait(growth_allocation_release);
        assert(r==0||r==PTHREAD_BARRIER_SERIAL_THREAD);
    }
    if (allocation_countdown == 0) { allocation_countdown = -1; return NULL; }
    if (allocation_countdown > 0) allocation_countdown--;
    void *memory = malloc(size);
    if (memory) allocations++;
    return memory;
}
static void kfree(void *memory) { if (memory) { frees++; free(memory); } }
static bool object_reference(kernel_object_t *object)
{ if (!object) return false; u32 count=__atomic_load_n(&object->ref_count,__ATOMIC_ACQUIRE); while(count&&count!=UINT32_MAX){if(__atomic_compare_exchange_n(&object->ref_count,&count,count+1U,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return true;}return false; }
static void object_release(kernel_object_t *object)
{ assert(object);u32 count=__atomic_fetch_sub(&object->ref_count,1U,__ATOMIC_ACQ_REL);assert(count); }
static void panic(const char *message) { fprintf(stderr,"%s\n",message);abort(); }
static process_t *object_process(kernel_object_t *object)
{ return (process_t *)((char *)object - offsetof(process_t, object)); }
static void spin_lock(spinlock_t *lock) { assert(pthread_mutex_lock(lock)==0);spin_depth++; }
static void spin_unlock(spinlock_t *lock) { assert(spin_depth);spin_depth--;assert(pthread_mutex_unlock(lock)==0); }
static u64 process_cli_save(void) { return 0; }
static void process_cli_restore(u64 flags) { (void)flags; }
static bool scheduler_block(void) { return block_result; }
static bool scheduler_sleep(u64 ticks) { (void)ticks;return sleep_result; }
static bool scheduler_thread_exit_complete(void *thread)
{ assert(thread);return thread_exit_complete; }
static void *thread_current(void) { return current_thread; }
static bool process_shutdown_requested(process_t *process)
{ (void)process;return shutdown_requested; }
static void process_unlink_locked(process_t *process) { process->parent=NULL; }
'''

NAMES = (
    "handle_slots", "process_handle_revision_advance", "process_handle_grow",
    "encode_handle", "decode_handle", "process_handle_install",
    "process_handle_reserve", "process_handle_activate",
    "process_handle_cancel", "process_handle_lookup_reference",
    "process_handle_lookup_any", "process_syscall_enter",
    "process_syscall_leave", "process_termination_begin",
    "process_termination_cancel",
    "process_handle_close", "process_handle_close_all", "process_wait",
)


def main():
    cc = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    with tempfile.TemporaryDirectory(prefix="mangrove-process-handles-") as temp:
        folder = Path(temp)
        code = PREFIX + "\n".join(function(name) for name in NAMES)
        code += r'''
static void process_init_fixture(process_t *process)
{
    memset(process, 0, sizeof(*process));
    process->state = PROCESS_STATE_ACTIVE;
    process->launch_ready = true;
    assert(pthread_mutex_init(&process->handle_lock, NULL) == 0);
    assert(pthread_mutex_init(&process->handle_growth_mutex.native, NULL) == 0);
    process->handle_growth_mutex.track_waiters = true;
    process->handle_capacity = PROCESS_HANDLE_SLOTS_INITIAL;
    process->handle_table = kmalloc(process->handle_capacity *
                                    sizeof(process_handle_slot_t));
    assert(process->handle_table);
    memset(process->handle_table, 0, process->handle_capacity *
           sizeof(process_handle_slot_t));
}
static bool lookup_matches(process_t *process, process_handle_t handle,
                           u32 rights)
{
    kernel_object_t *object = process_handle_lookup_any(
        process, handle, OBJECT_TYPE_TEST, rights);
    if (!object) return false;
    bool matches = object->type == OBJECT_TYPE_TEST;
    object_release(object);
    return matches;
}
typedef struct {
    process_t *process;
    pthread_barrier_t entered;
    pthread_barrier_t release;
    bool result;
} syscall_hold_t;
static void *hold_syscall(void *opaque)
{
    syscall_hold_t *hold = opaque;
    hold->result = process_syscall_enter(hold->process);
    int result = pthread_barrier_wait(&hold->entered);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    result = pthread_barrier_wait(&hold->release);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    if (hold->result) process_syscall_leave(hold->process);
    return NULL;
}
typedef struct {
    process_t *process;
    process_handle_t handle;
    pthread_barrier_t attempted;
    _Atomic bool result;
} close_job_t;
static void *close_worker(void *opaque)
{
    close_job_t *job = opaque;
    int result = pthread_barrier_wait(&job->attempted);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    job->result = process_handle_close(job->process, job->handle);
    return NULL;
}
typedef struct {
    process_t *process;
    kernel_object_t *object;
    process_handle_t handle;
    bool result;
} grow_job_t;
static void *grow_worker(void *opaque)
{
    grow_job_t *job = opaque;
    job->result = process_handle_install(job->process, job->object,
                                         OBJECT_RIGHT_READ, &job->handle);
    return NULL;
}
static void process_fini_fixture(process_t *process)
{
    assert(pthread_mutex_destroy(&process->handle_lock) == 0);
    assert(pthread_mutex_destroy(&process->handle_growth_mutex.native) == 0);
}
int main(void)
{
    process_t process;
    kernel_object_t object = {OBJECT_TYPE_TEST, 1};
    process_handle_t handles[PROCESS_HANDLE_SLOTS_MAX];
    process_init_fixture(&process);

    /* Growth across several capacities preserves all active handles. */
    for (usize i = 0; i < 4097; i++) {
        assert(process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                      &handles[i]));
        assert(lookup_matches(&process, handles[i], OBJECT_RIGHT_READ));
    }
    assert(process.handle_capacity >= 4097 && process.handle_capacity <
           PROCESS_HANDLE_SLOTS_MAX);
    for (usize i = 0; i < 4097; i++)
        assert(lookup_matches(&process, handles[i], 0));

    /* A failed growth leaves all existing handles usable and does not retain
       the attempted object's reference. */
    process_handle_close_all(&process);
    assert(object.ref_count == 1);
    kfree(process.handle_table);
    process_fini_fixture(&process);
    memset(&process, 0, sizeof(process));
    process_init_fixture(&process);
    for (usize i = 0; i < PROCESS_HANDLE_SLOTS_INITIAL; i++)
        assert(process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                      &handles[i]));
    allocation_countdown = 0;
    assert(!process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                   &handles[PROCESS_HANDLE_SLOTS_INITIAL]));
    assert(object.ref_count == PROCESS_HANDLE_SLOTS_INITIAL + 1U);
    for (usize i = 0; i < PROCESS_HANDLE_SLOTS_INITIAL; i++)
        assert(lookup_matches(&process, handles[i], OBJECT_RIGHT_READ));
    assert(process_handle_close(&process, handles[3]));
    assert(!process_handle_lookup_any(&process, handles[3],
           OBJECT_TYPE_TEST, 0));
    assert(process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                  &handles[16]));
    assert(handles[16] != handles[3]);
    assert(lookup_matches(&process, handles[16], 0));

    /* Reserved handles remain invisible until activation and can be
       cancelled without leaking the transferred object reference. */
    object.ref_count++;
    process_handle_t reserved;
    assert(process_handle_reserve(&process, &object, OBJECT_RIGHT_WRITE,
                                  &reserved));
    assert(!process_handle_lookup_any(&process, reserved,
           OBJECT_TYPE_TEST, 0));
    assert(process_handle_activate(&process, reserved));
    assert(lookup_matches(&process, reserved, OBJECT_RIGHT_WRITE));
    assert(process_handle_close(&process, reserved));
    object.ref_count++;
    assert(process_handle_reserve(&process, &object, OBJECT_RIGHT_READ,
                                  &reserved));
    assert(process_handle_cancel(&process, reserved));
    assert(!process_handle_cancel(&process, reserved));

    process_handle_close_all(&process);
    assert(object.ref_count == 1);
    assert(process.handle_capacity == 32);
    kfree(process.handle_table);
    process_fini_fixture(&process);

    /* The handle ABI encodes index+1 in 16 bits, so all 65,535 representable
       slots are usable and the next request fails without corrupting them. */
    process_init_fixture(&process);
    for (usize i = 0; i < PROCESS_HANDLE_SLOTS_MAX; i++)
        assert(process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                      &handles[i]));
    assert(process.handle_capacity == PROCESS_HANDLE_SLOTS_MAX);
    assert(!process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                   &handles[0]));
    assert(object.ref_count == PROCESS_HANDLE_SLOTS_MAX + 1U);
    assert(lookup_matches(&process, handles[PROCESS_HANDLE_SLOTS_MAX-1], 0));
    process_handle_close_all(&process);
    assert(object.ref_count == 1);
    kfree(process.handle_table);

    /* A successful lookup owns a reference independently of the table slot.
       Closing the handle cannot invalidate an operation already in flight. */
    process_fini_fixture(&process);
    process_init_fixture(&process);
    process_handle_t live;
    assert(process_handle_install(&process,&object,OBJECT_RIGHT_READ,&live));
    kernel_object_t *held=process_handle_lookup_any(&process,live,
        OBJECT_TYPE_TEST,OBJECT_RIGHT_READ);
    assert(held==&object&&__atomic_load_n(&object.ref_count,__ATOMIC_ACQUIRE)==3U);
    assert(process_handle_close(&process,live));
    assert(__atomic_load_n(&object.ref_count,__ATOMIC_ACQUIRE)==2U);
    object_release(held);
    assert(__atomic_load_n(&object.ref_count,__ATOMIC_ACQUIRE)==1U);
    process_handle_close_all(&process);
    kfree(process.handle_table);
    process_fini_fixture(&process);

    /* A coordinated in-flight syscall blocks process termination admission;
       once termination owns the gate, a new syscall cannot enter. */
    memset(&process,0,sizeof(process));
    process.state=PROCESS_STATE_ACTIVE;
    process.launch_ready=true;
    process.main_thread=(void *)(uintptr_t)1;
    process.object.type=OBJECT_TYPE_TEST;
    process.object.ref_count=1;
    syscall_hold_t syscall_hold={.process=&process};
    assert(pthread_barrier_init(&syscall_hold.entered,NULL,2)==0);
    assert(pthread_barrier_init(&syscall_hold.release,NULL,2)==0);
    pthread_t syscall_thread;
    assert(pthread_create(&syscall_thread,NULL,hold_syscall,&syscall_hold)==0);
    int barrier_result=pthread_barrier_wait(&syscall_hold.entered);
    assert(barrier_result==0||barrier_result==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(syscall_hold.result&&process.active_syscalls==1U&&
           process.object.ref_count==2U);
    assert(!process_termination_begin(&process));
    barrier_result=pthread_barrier_wait(&syscall_hold.release);
    assert(barrier_result==0||barrier_result==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(pthread_join(syscall_thread,NULL)==0);
    assert(process.active_syscalls==0U&&process.object.ref_count==1U);
    assert(process_termination_begin(&process));
    assert(!process_syscall_enter(&process));
    process_termination_cancel(&process);
    assert(process_syscall_enter(&process));
    process_syscall_leave(&process);
    assert(pthread_barrier_destroy(&syscall_hold.entered)==0&&
           pthread_barrier_destroy(&syscall_hold.release)==0);

    /* During growth, the writer mutex keeps a concurrent close from changing
       the source slots while the production copy is paused at allocation. */
    process_init_fixture(&process);
    for(usize i=0;i<PROCESS_HANDLE_SLOTS_INITIAL;i++)
        assert(process_handle_install(&process,&object,OBJECT_RIGHT_READ,&handles[i]));
    pthread_barrier_t allocation_entered,allocation_release;
    assert(pthread_barrier_init(&allocation_entered,NULL,2)==0);
    assert(pthread_barrier_init(&allocation_release,NULL,2)==0);
    growth_allocation_entered=&allocation_entered;
    growth_allocation_release=&allocation_release;
    pause_growth_allocation=true;
    grow_job_t grow={.process=&process,.object=&object};
    pthread_t growth_thread;
    assert(pthread_create(&growth_thread,NULL,grow_worker,&grow)==0);
    barrier_result=pthread_barrier_wait(&allocation_entered);
    assert(barrier_result==0||barrier_result==PTHREAD_BARRIER_SERIAL_THREAD);
    /* A lookup continues to retain from the published old table while the
       growth worker owns the writer mutex and is preparing its snapshot. */
    assert(lookup_matches(&process,handles[1],OBJECT_RIGHT_READ));
    close_job_t close={.process=&process,.handle=handles[0]};
    assert(pthread_barrier_init(&close.attempted,NULL,2)==0);
    pthread_t close_thread;
    assert(pthread_create(&close_thread,NULL,close_worker,&close)==0);
    barrier_result=pthread_barrier_wait(&close.attempted);
    assert(barrier_result==0||barrier_result==PTHREAD_BARRIER_SERIAL_THREAD);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME,&deadline)==0);
    deadline.tv_sec++;
    while(!__atomic_load_n(&process.handle_growth_mutex.waiters,
                            __ATOMIC_ACQUIRE)) {
        struct timespec now;
        assert(clock_gettime(CLOCK_REALTIME,&now)==0);
        assert(now.tv_sec<deadline.tv_sec ||
               (now.tv_sec==deadline.tv_sec && now.tv_nsec<deadline.tv_nsec));
        sched_yield();
    }
    assert(!__atomic_load_n(&close.result,__ATOMIC_ACQUIRE));
    barrier_result=pthread_barrier_wait(&allocation_release);
    assert(barrier_result==0||barrier_result==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(pthread_join(growth_thread,NULL)==0);
    assert(pthread_join(close_thread,NULL)==0);
    growth_allocation_entered=growth_allocation_release=NULL;
    assert(grow.result&&close.result&&process.handle_capacity==32U);
    assert(lookup_matches(&process,grow.handle,OBJECT_RIGHT_READ));
    process_handle_close_all(&process);
    assert(__atomic_load_n(&object.ref_count,__ATOMIC_ACQUIRE)==1U);
    kfree(process.handle_table);
    process_fini_fixture(&process);
    assert(pthread_barrier_destroy(&allocation_entered)==0&&
           pthread_barrier_destroy(&allocation_release)==0&&
           pthread_barrier_destroy(&close.attempted)==0);

    /* A retained child-process handle is released on both early wake failure
       and interrupted teardown-wait paths, as well as on successful reap. */
    process_t parent={0},child={0};
    process_init_fixture(&parent);
    parent.main_thread=(void *)(uintptr_t)2;
    memset(&child,0,sizeof(child));
    child.state=PROCESS_STATE_ACTIVE;child.main_thread=(void *)(uintptr_t)3;
    child.parent=&parent;child.object.type=OBJECT_TYPE_PROCESS;
    child.object.ref_count=1;child.owner_reference_held=true;
    process_handle_t child_handle;
    assert(process_handle_install(&parent,&child.object,0,&child_handle));
    current_thread=(void *)(uintptr_t)4;block_result=true;
    thread_exit_complete=false;sleep_result=false;shutdown_requested=false;
    i32 child_status=0;
    assert(process_wait(&parent,child_handle,&child_status)==MG_ERR_BUSY);
    assert(__atomic_load_n(&child.object.ref_count,__ATOMIC_ACQUIRE)==2U);
    assert(parent.waiting_child==NULL&&parent.waiting_thread==NULL);
    child.state=PROCESS_STATE_TERMINATED;
    assert(process_wait(&parent,child_handle,&child_status)==MG_ERR_BUSY);
    assert(__atomic_load_n(&child.object.ref_count,__ATOMIC_ACQUIRE)==2U);
    thread_exit_complete=true;child.exit_status=37;
    assert(process_wait(&parent,child_handle,&child_status)==0);
    assert(child_status==37&&child.wait_collected&&
           __atomic_load_n(&child.object.ref_count,__ATOMIC_ACQUIRE)==1U);
    assert(process_handle_close(&parent,child_handle));
    assert(__atomic_load_n(&child.object.ref_count,__ATOMIC_ACQUIRE)==0U);
    process_handle_close_all(&parent);kfree(parent.handle_table);
    process_fini_fixture(&parent);
    assert(allocations == frees);
    puts("Process handles/syscall lifetime: retained lookups, coordinated growth/close, admission, generation and exhaustion PASS");
    return 0;
}
'''
        fixture = folder / "fixture.c"
        fixture.write_text(code)
        binary = folder / "test"
        subprocess.run([cc, "-std=gnu17", "-O1", "-g", "-Wall", "-Wextra",
            "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            str(fixture), "-o", str(binary)], check=True, cwd=ROOT, timeout=60)
        subprocess.run([str(binary)], check=True, cwd=ROOT, timeout=60,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


if __name__ == "__main__":
    main()
