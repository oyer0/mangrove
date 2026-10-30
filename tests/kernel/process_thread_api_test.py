#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production worker creation, join/detach and reclamation fixture."""
from pathlib import Path
import os
import re
import resource
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
sys.path.insert(0, str(ROOT / "tests/shared"))
from host_fixture import HostFixture


def function(path, name):
    source = (ROOT / path).read_text()
    pattern = r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\("
    for match in re.finditer(pattern, source, re.M):
        start = source.index("{", match.end())
        if ";" in source[match.end():start]:
            continue
        end, depth = start + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise ValueError(f"missing production function: {path}:{name}")


prefix = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint16_t u16;
typedef int32_t i32;
typedef int64_t i64;
typedef size_t usize;
typedef uintptr_t phys_addr_t;
typedef pthread_mutex_t spinlock_t;
typedef struct kernel_mutex mutex_t;
typedef u32 process_handle_t;
typedef i64 mg_result_t;
typedef u64 mg_thread_result_t;
typedef struct {process_handle_t handle;u64 id;} mg_thread_created_t;
typedef struct process process_t;
typedef struct kernel_thread kernel_thread_t;
typedef struct kernel_object kernel_object_t;
typedef struct process_user_region process_user_region_t;
typedef struct process_thread_record process_thread_record_t;
struct kernel_mutex {
    spinlock_t state_lock;
    kernel_thread_t *owner, *wait_head, *wait_tail;
};
typedef enum { PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING,
               PROCESS_STATE_TERMINATED } process_state_t;
typedef enum { THREAD_PROCESS_UNATTACHED, THREAD_PROCESS_ATTACHED,
               THREAD_PROCESS_EXIT_REQUESTED,
               THREAD_PROCESS_HANDOFF_COMPLETE } thread_process_state_t;
typedef enum { THREAD_STATE_READY, THREAD_STATE_RUNNING,
               THREAD_STATE_BLOCKED, THREAD_STATE_TERMINATED } thread_state_t;
typedef enum { SCHEDULER_WAKE_FAILED=0, SCHEDULER_WAKE_DELIVERED,
               SCHEDULER_WAKE_ALREADY_TERMINATED } scheduler_wake_result_t;
typedef enum { OBJECT_TYPE_INVALID, OBJECT_TYPE_PROCESS, OBJECT_TYPE_THREAD }
    kernel_object_type_t;
enum { MG_OK=0, MG_ERR_INVALID_HANDLE=-3, MG_ERR_BAD_ARGUMENT=-4,
       MG_ERR_NO_MEMORY=-6, MG_ERR_BUSY=-14, MG_ERR_INVALID_EXEC=-15,
       MG_ERR_SHUTDOWN=-38, MG_ERR_INTERRUPTED=-39 };
enum { MG_PROCESS_FLAG_WORKER_REAP_PENDING=1U<<2,
       MG_PROCESS_FLAG_USER_REGION_REAP_PENDING=1U<<3,
       MG_PROCESS_FLAG_WORKER_REAPER_ACTIVE=1U<<4,
       MG_PROCESS_FLAG_REAP_STATE_MASK=(1U<<2)|(1U<<3)|(1U<<4) };
enum { PROCESS_USER_REGION_INITIAL_STACK=1,
       PROCESS_USER_REGION_WORKER_STACK,
       PROCESS_USER_REGION_THREAD_TLS };
typedef enum { PROCESS_REGION_RELEASE_INVALID,
               PROCESS_REGION_RELEASE_DEFERRED,
               PROCESS_REGION_RELEASED } process_region_release_result_t;
typedef enum { VMM_USER_RANGE_REVOKE_INVALID,
               VMM_USER_RANGE_REVOKE_RETRY,
               VMM_USER_RANGE_REVOKED } vmm_user_range_revoke_result_t;
enum { THREAD_CPU_NONE = UINT32_MAX };
struct kernel_object {
    u32 ref_count;
    kernel_object_type_t type;
    void (*destroy)(kernel_object_t *);
};
struct process_user_region {
    uintptr_t reservation_base, mapping_base;
    usize reservation_pages, mapping_pages;
    u32 kind, leases;
    bool worker_owned, revoked;
    bool release_pending;
    process_user_region_t *next;
};
typedef struct { bool valid; process_user_region_t *stack_region,*tls_region;
                 uintptr_t instruction_pointer,stack_pointer,rdi,rsi,fs_base; }
    user_execution_context_t;
struct kernel_thread {
    thread_state_t state;
    u32 running_cpu;
    bool context_switch_pending, syscall_active, sleeping, stack_external;
    uintptr_t preempt_return_rip;
    u64 id;
    process_t *process;
    kernel_thread_t *process_next;
    thread_process_state_t process_state;
    bool process_initial, process_reference_held, process_exit_requested;
    i32 process_exit_status;
    bool process_exit_status_valid, kernel_stack_active, queued;
    usize kernel_stack_size;
    uintptr_t kernel_stack_base, saved_stack_pointer;
    user_execution_context_t user_context;
    uintptr_t user_fs_base;
    process_thread_record_t *worker_record;
    bool event_wake_pending;
    mutex_t *waiting_mutex;
    kernel_thread_t *mutex_wait_next;
    bool mutex_wake_pending;
};
struct process_thread_record {
    kernel_object_t object;
    process_t *process;
    kernel_thread_t *execution,*join_waiter;
    process_user_region_t *stack_region,*tls_region;
    process_thread_record_t *next;
    u64 id,result;
    bool completed,kernel_stack_released,join_claimed,joined,detached,
         listed,reclaiming;
};
typedef struct { kernel_object_t *object; u32 rights; u16 generation;
                 bool active,reserved; } process_handle_slot_t;
struct process {
    kernel_object_t object;
    process_state_t state;
    bool exit_requested, termination_pending, launch_ready;
    bool exit_cleanup_complete, exit_cleanup_in_progress;
    bool thread_reap_in_progress,worker_reap_pending;
    bool user_region_reap_pending;
    u32 active_syscalls,thread_count,live_thread_count,thread_list_readers;
    u32 scheduler_handoff_references;
    _Atomic u32 scheduler_handoff_releases_pending;
    u32 worker_reapers;
    kernel_thread_t *main_thread,*threads;
    process_thread_record_t *thread_records;
    process_user_region_t *user_regions;
    u32 storage_authorized_operation;
    u64 storage_authorized_thread_id;
    bool storage_management_session, storage_management_authorizing;
    void *address_space;
    bool image_loaded,tls_present;
    kernel_object_t *handle_table;
    usize handle_capacity,next_handle_slot;
    u64 handle_revision;
    spinlock_t handle_lock;
    mutex_t handle_growth_mutex,memory_lock;
};
static spinlock_t process_registry_lock=PTHREAD_MUTEX_INITIALIZER;
static spinlock_t scheduler_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local kernel_thread_t *local_thread;
static pthread_mutex_t event_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_changed=PTHREAD_COND_INITIALIZER;
static bool block_entered;
static bool reap_race_armed,reap_race_paused,reap_race_done;
static bool lost_race_armed,lost_race_paused,lost_race_done;
static bool create_reap_after_publish;
static process_handle_t last_reserved_handle;
static bool scheduler_enqueue_failure;
static bool wake_delivery_pause_armed,wake_delivery_paused;
static bool wake_delivery_resume;
static kernel_thread_t *registry_attempt_watch;
static bool registry_attempt_seen;
static usize allocation_count,fail_allocation_at;
static unsigned created_threads,destroyed_threads,stack_releases;
static unsigned region_releases,handle_cancels;
static bool fail_thread_create,fail_prepare,fail_reserve,fail_enqueue;
static u32 lease_on_revoke_kind;
static bool pause_prepare,prepare_entered,allow_prepare;
static bool interrupt_pending;
static bool refuse_scheduler_block;
static process_user_region_t *leased_region;
static mutex_t vmm_operation_mutex;
static kernel_thread_t *vmm_operation_owner;
static bool vmm_operation_contended;
static u64 next_thread_id=100;
static _Thread_local u64 fixture_eflags=1ULL<<9;
static process_t *test_process;
void mutex_init(mutex_t *);
bool mutex_lock(mutex_t *);
bool mutex_unlock(mutex_t *);
static scheduler_wake_result_t scheduler_wake_waiter_result(
    kernel_thread_t *);
static bool scheduler_unblock_locked(kernel_thread_t *);
static bool scheduler_consume_pending_wake_locked(kernel_thread_t *,bool);
static process_region_release_result_t process_user_region_release(
    process_t*,process_user_region_t*);
static bool process_user_region_set_worker_owned(process_t*,
    process_user_region_t*,process_user_region_t*,bool);
static void process_worker_reclaim_pending(process_t*);
static int process_thread_detach(process_t*,process_handle_t);
static void process_worker_exit(struct kernel_thread*,u64);
static void process_scheduler_handoff_complete(struct kernel_thread*);
static void worker_complete(process_thread_record_t*,u64);
static void test_after_created_handle_is_visible(void);
static void process_user_thread_entry(void *argument){(void)argument;}
static void panic(const char *message)
{fprintf(stderr,"unexpected panic: %s\n",message);abort();}

static u64 spin_lock_irqsave(spinlock_t *lock)
{ if(lock==&process_registry_lock&&registry_attempt_watch&&
      local_thread==registry_attempt_watch){
    pthread_mutex_lock(&event_lock);registry_attempt_seen=true;
    pthread_cond_broadcast(&event_changed);pthread_mutex_unlock(&event_lock);
  }
  assert(pthread_mutex_lock(lock)==0); return 0; }
static void spin_unlock_irqrestore(spinlock_t *lock,u64 flags)
{ (void)flags; assert(pthread_mutex_unlock(lock)==0);
  if(lock==&scheduler_lock){
   pthread_mutex_lock(&event_lock);pthread_cond_broadcast(&event_changed);
   if(wake_delivery_pause_armed){wake_delivery_pause_armed=false;
    wake_delivery_paused=true;pthread_cond_broadcast(&event_changed);
    while(!wake_delivery_resume)
     pthread_cond_wait(&event_changed,&event_lock);
   }
   pthread_mutex_unlock(&event_lock);
  }
  if(lock==&process_registry_lock&&reap_race_armed&&test_process&&
     test_process->thread_records){
   process_thread_record_t*r=test_process->thread_records;
   if(r->reclaiming&&!r->detached&&!r->joined&&r->kernel_stack_released&&
      !r->stack_region&&!r->tls_region&&r->execution){
    pthread_mutex_lock(&event_lock);reap_race_armed=false;
    reap_race_paused=true;pthread_cond_broadcast(&event_changed);
    while(!reap_race_done)pthread_cond_wait(&event_changed,&event_lock);
    pthread_mutex_unlock(&event_lock);
   }
  }
  if(lock==&process_registry_lock&&lost_race_armed&&test_process&&
     test_process->thread_records){
   process_thread_record_t*r=test_process->thread_records;
   if(r->listed&&r->completed&&!r->reclaiming&&!r->execution&&
      r->kernel_stack_released&&!r->stack_region&&!r->tls_region&&
      !r->joined&&!r->detached&&!test_process->worker_reapers){
    pthread_mutex_lock(&event_lock);lost_race_armed=false;
    lost_race_paused=true;pthread_cond_broadcast(&event_changed);
    while(!lost_race_done)pthread_cond_wait(&event_changed,&event_lock);
    pthread_mutex_unlock(&event_lock);
   }
  }
 }
static void spinlock_init(spinlock_t *lock)
{assert(pthread_mutex_init(lock,NULL)==0);}
static void spin_lock(spinlock_t *lock)
{assert(pthread_mutex_lock(lock)==0);}
static void spin_unlock(spinlock_t *lock)
{assert(pthread_mutex_unlock(lock)==0);}
static void *kmalloc(usize size)
{ allocation_count++; if(fail_allocation_at==allocation_count)return NULL;
  return malloc(size); }
static void kfree(void *p){free(p);}
static void object_init(kernel_object_t *o,kernel_object_type_t type,
                        void(*destroy)(kernel_object_t*))
{o->ref_count=1;o->type=type;o->destroy=destroy;}
static bool object_reference(kernel_object_t *o)
{u32 n;if(!o)return false;n=__atomic_load_n(&o->ref_count,__ATOMIC_RELAXED);
 while(n && n!=UINT32_MAX) {
  if(__atomic_compare_exchange_n(&o->ref_count,&n,n+1,false,__ATOMIC_ACQUIRE,
                                 __ATOMIC_RELAXED)) return true;
 }
 return false;}
static void object_release(kernel_object_t *o)
{u32 n;if(!o)return;n=__atomic_sub_fetch(&o->ref_count,1,__ATOMIC_ACQ_REL);
 assert(n!=UINT32_MAX);if(!n&&o->destroy)o->destroy(o);}
static void process_handle_reference_release(kernel_object_t *o)
{object_release(o);}
static bool scheduler_thread_process_attachable(const kernel_thread_t *t)
{return t&&t->state==THREAD_STATE_READY&&t->running_cpu==THREAD_CPU_NONE&&
 !t->context_switch_pending&&!t->syscall_active&&!t->sleeping&&
 !t->preempt_return_rip;}
static bool scheduler_thread_initial_process_attachable(const kernel_thread_t*t)
{return t&&t==local_thread&&t->state==THREAD_STATE_RUNNING&&
 t->running_cpu!=THREAD_CPU_NONE&&!t->context_switch_pending;}
static bool scheduler_thread_exit_complete(const kernel_thread_t *t)
{return t&&t->state==THREAD_STATE_TERMINATED&&
 t->running_cpu==THREAD_CPU_NONE&&!t->context_switch_pending&&
 !t->sleeping&&!t->preempt_return_rip;}
static bool scheduler_thread_handoff_callback_valid(
 const kernel_thread_t*t)
{return t&&t->state==THREAD_STATE_TERMINATED&&
 t->running_cpu==THREAD_CPU_NONE&&t->context_switch_pending&&
 !t->sleeping&&!t->preempt_return_rip;}
static bool kernel_stack_release_context_valid(void){return true;}
static bool vmm_user_range_executable(void *as,uintptr_t p,usize n)
{return as&&p&&n;}
static process_handle_slot_t *handle_slots(process_t *p)
{return (process_handle_slot_t*)p->handle_table;}
static process_handle_t encode_handle(u32 i,u16 g)
{return ((u32)g<<16)|(i+1U);}
static bool decode_handle(process_handle_t h,u32 *i,u16 *g)
{u32 low=h&0xffffU;if(!h||!low)return false;*i=low-1U;*g=(u16)(h>>16);return *g!=0;}
static bool process_handle_reserve(process_t *p,kernel_object_t *o,u32 rights,
                                  process_handle_t *out)
{u64 f;process_handle_slot_t *s;if(fail_reserve)return false;
 f=spin_lock_irqsave(&p->handle_lock);s=handle_slots(p);
 for(usize i=0;i<p->handle_capacity;i++)if(!s[i].active&&!s[i].reserved){
  u16 g=(u16)(s[i].generation+1U);if(!g)g=1;s[i].object=o;s[i].rights=rights;
  s[i].generation=g;s[i].reserved=true;p->next_handle_slot=i+1U;
  *out=encode_handle((u32)i,g);last_reserved_handle=*out;
  spin_unlock_irqrestore(&p->handle_lock,f);
  return true;}
 spin_unlock_irqrestore(&p->handle_lock,f);return false;}
static bool process_handle_cancel(process_t *p,process_handle_t h)
{u32 i;u16 g;kernel_object_t *o;u64 f;if(!decode_handle(h,&i,&g))return false;
 f=spin_lock_irqsave(&p->handle_lock);if(i>=p->handle_capacity){
 spin_unlock_irqrestore(&p->handle_lock,f);return false;}
 process_handle_slot_t*s=&handle_slots(p)[i];if(!s->reserved||s->active||
 s->generation!=g){spin_unlock_irqrestore(&p->handle_lock,f);return false;}
 o=s->object;s->object=NULL;s->reserved=false;s->rights=0;
 spin_unlock_irqrestore(&p->handle_lock,f);handle_cancels++;object_release(o);
 return true;}
static bool process_handle_close(process_t *,process_handle_t);
static kernel_thread_t *thread_create_suspended(const char *,void(*)(void*),void*);
static bool scheduler_enqueue(kernel_thread_t *t)
{if(fail_enqueue)return false;t->queued=true;return true;}
static bool process_user_context_prepare_worker(process_t *p,kernel_thread_t*t,
                                                uintptr_t entry,uintptr_t arg)
{if(pause_prepare){pthread_mutex_lock(&event_lock);prepare_entered=true;
 pthread_cond_broadcast(&event_changed);
 while(!allow_prepare) pthread_cond_wait(&event_changed,&event_lock);
 pthread_mutex_unlock(&event_lock);}
 if(fail_prepare)return false;
 process_user_region_t *s=calloc(1,sizeof(*s)),*tls=calloc(1,sizeof(*tls));
 if(!s||!tls){free(s);free(tls);return false;}
 s->kind=PROCESS_USER_REGION_WORKER_STACK;s->mapping_base=0x10000000+t->id*0x10000;
 s->mapping_pages=8;tls->kind=PROCESS_USER_REGION_THREAD_TLS;
 tls->mapping_base=s->mapping_base+0x10000;tls->mapping_pages=1;
 s->next=p->user_regions;p->user_regions=s;tls->next=p->user_regions;
 p->user_regions=tls;t->user_context.valid=true;t->user_context.stack_region=s;
 t->user_context.tls_region=tls;t->user_context.instruction_pointer=entry;
 t->user_context.rdi=arg;t->user_context.stack_pointer=s->mapping_base+8*4096;
 t->user_fs_base=tls->mapping_base;return true;}
static bool process_user_context_discard_worker(process_t*p,kernel_thread_t*t);
static bool process_publish_terminated_locked(process_t*p)
{(void)p;return false;}
static bool process_cleanup_exit_resources(process_t*p){(void)p;return true;}
static bool scheduler_unblock_locked(kernel_thread_t*t)
{if(!t||t->state!=THREAD_STATE_BLOCKED||t->queued||
    scheduler_enqueue_failure)return false;
 t->state=THREAD_STATE_READY;t->queued=true;t->sleeping=false;return true;}
static bool scheduler_wake_waiter(kernel_thread_t*t)
{return scheduler_wake_waiter_result(t)==SCHEDULER_WAKE_DELIVERED;}
static bool scheduler_mutex_wake(kernel_thread_t*t)
{if(!t)return false;
 __atomic_store_n(&t->mutex_wake_pending,true,__ATOMIC_RELEASE);
 if(t->state==THREAD_STATE_BLOCKED)t->state=THREAD_STATE_READY;
 pthread_mutex_lock(&event_lock);pthread_cond_broadcast(&event_changed);
 pthread_mutex_unlock(&event_lock);return true;}
static bool scheduler_block(void)
{kernel_thread_t*t=local_thread;if(refuse_scheduler_block)return false;
 pthread_mutex_lock(&event_lock);
 block_entered=true;pthread_cond_broadcast(&event_changed);
 while(!__atomic_load_n(&t->event_wake_pending,__ATOMIC_ACQUIRE)&&
       !__atomic_load_n(&t->mutex_wake_pending,__ATOMIC_ACQUIRE))
 pthread_cond_wait(&event_changed,&event_lock);
 (void)__atomic_exchange_n(&t->event_wake_pending,false,__ATOMIC_ACQ_REL);
 pthread_mutex_unlock(&event_lock);return true;}
static void scheduler_clear_event_wake(kernel_thread_t*t)
{if(t)__atomic_store_n(&t->event_wake_pending,false,__ATOMIC_RELEASE);}
static bool process_take_interrupt(process_t*p){(void)p;return
 __atomic_exchange_n(&interrupt_pending,false,__ATOMIC_ACQ_REL);}
static bool process_shutdown_requested(const process_t*p){(void)p;return false;}
static bool process_thread_exit_requested(const kernel_thread_t*t)
{return t&&__atomic_load_n(&t->process_exit_requested,__ATOMIC_ACQUIRE);}
static u64 process_cli_save(void)
{u64 f=fixture_eflags;fixture_eflags&=~(1ULL<<9);return f;}
static void process_cli_restore(u64 f){fixture_eflags=f;}
static u64 process_current_thread_id(void){return local_thread?local_thread->id:0;}
static kernel_thread_t *thread_current(void){return local_thread;}
static kernel_thread_t *thread_create_suspended(const char*n,
                                                 void(*entry)(void*),void*a)
{(void)n;(void)entry;(void)a;if(fail_thread_create)return NULL;
 kernel_thread_t*t=calloc(1,sizeof(*t));if(!t)return NULL;
 t->state=THREAD_STATE_READY;t->running_cpu=THREAD_CPU_NONE;t->id=++next_thread_id;
 t->kernel_stack_active=true;t->kernel_stack_size=65536;
 t->kernel_stack_base=0x70000000+t->id*0x10000;created_threads++;return t;}
static bool thread_destroy(kernel_thread_t*t)
{if(!t||t->process||t->process_reference_held)return false;
 destroyed_threads++;free(t);return true;}
static bool thread_release_terminated_stack(kernel_thread_t*t)
{if(!scheduler_thread_exit_complete(t))return false;
 if(t->kernel_stack_active){t->kernel_stack_active=false;stack_releases++;}
 return true;}
static bool vmm_unmap_user_range(void*as,void*base,usize pages)
{(void)as;(void)pages;
 if(vmm_operation_contended && vmm_operation_owner!=local_thread)
  vmm_operation_mutex.owner=vmm_operation_owner;
 else vmm_operation_mutex.owner=NULL;
 if(!mutex_lock(&vmm_operation_mutex))return false;
 if(leased_region&&leased_region->mapping_base==(uintptr_t)base){
  mutex_unlock(&vmm_operation_mutex);return false;}
 region_releases++;mutex_unlock(&vmm_operation_mutex);return true;}
static vmm_user_range_revoke_result_t vmm_user_range_revoke(
    void*as,void*base,usize pages)
{(void)as;(void)pages;
 if(vmm_operation_contended && vmm_operation_owner!=local_thread)
  vmm_operation_mutex.owner=vmm_operation_owner;
 else vmm_operation_mutex.owner=NULL;
 /* Use production mutex_lock(): during scheduler post-switch, handoff_mutex
  * is already owned by local_thread but its waiting marker has not yet been
  * consumed. A direct VMM revoke refuses the contended operation mutex; the
  * scheduler reaper itself defers before acquiring any mutex. */
 if(!mutex_lock(&vmm_operation_mutex))
  return VMM_USER_RANGE_REVOKE_RETRY;
 for(process_user_region_t*r=test_process->user_regions;r;
 r=r->next)if(r->mapping_base==(uintptr_t)base){
  if(lease_on_revoke_kind&&r->kind==lease_on_revoke_kind){
   leased_region=r;lease_on_revoke_kind=0;}
  mutex_unlock(&vmm_operation_mutex);
  return VMM_USER_RANGE_REVOKED;}
 mutex_unlock(&vmm_operation_mutex);
 return VMM_USER_RANGE_REVOKE_INVALID;}
static bool process_user_context_discard_worker(process_t*p,kernel_thread_t*t)
{
 if(!process_user_region_set_worker_owned(p,t->user_context.stack_region,
       t->user_context.tls_region,false))return false;
 if(t->user_context.tls_region&&process_user_region_release(
      p,t->user_context.tls_region)==PROCESS_REGION_RELEASE_INVALID)
     return false;
 if(t->user_context.stack_region&&process_user_region_release(
      p,t->user_context.stack_region)==PROCESS_REGION_RELEASE_INVALID)
     return false;
 memset(&t->user_context,0,sizeof(t->user_context));t->user_fs_base=0;
 return true;
}
'''

names = (
    ("kernel/src/process.c", "process_handle_revision_advance"),
    ("kernel/src/process.c", "process_thread_register_locked"),
    ("kernel/src/process.c", "process_thread_member_locked"),
    ("kernel/src/process.c", "process_thread_record_destroy"),
    ("kernel/src/process.c", "process_snapshot_reap_flags_locked"),
    ("kernel/src/process.c", "process_user_region_release"),
    ("kernel/src/process.c", "process_user_regions_reap_pending"),
    ("kernel/src/process.c", "process_user_region_set_worker_owned"),
    ("kernel/src/process.c", "process_thread_handle_activate_locked"),
    ("kernel/src/process.c", "process_thread_handle_deactivate_locked"),
    ("kernel/src/process.c", "process_thread_create"),
    ("kernel/src/process.c", "process_storage_authorization_revoke_thread_locked"),
    ("kernel/src/process.c", "process_worker_exit"),
    ("kernel/src/process.c", "process_detach_thread_owned"),
    ("kernel/src/process.c", "process_detach_thread"),
    ("kernel/src/process.c", "process_scheduler_handoff_complete_internal"),
    ("kernel/src/process.c", "process_scheduler_handoff_complete"),
    ("kernel/src/process.c", "process_worker_reap_needed_locked"),
    ("kernel/src/process.c", "process_worker_reap_immediate_locked"),
    ("kernel/src/process.c", "process_worker_reclaim_context_valid"),
    ("kernel/src/process.c", "process_worker_reclaim_pending"),
    ("kernel/src/process.c", "process_thread_record_from_object"),
    ("kernel/src/process.c", "process_thread_handle_close_allowed"),
    ("kernel/src/process.c", "process_handle_lookup_reference"),
    ("kernel/src/process.c", "process_handle_lookup"),
    ("kernel/src/process.c", "process_thread_join"),
    ("kernel/src/process.c", "process_thread_detach"),
    ("kernel/src/process.c", "process_handle_close"),
    ("kernel/src/scheduler.c", "scheduler_wake_waiter_result"),
    ("kernel/src/scheduler.c", "scheduler_consume_pending_wake_locked"),
)

suffix = r'''
static void init(process_t*p,kernel_thread_t*main_thread)
{memset(p,0,sizeof(*p));memset(main_thread,0,sizeof(*main_thread));
 p->object.ref_count=2;p->object.type=OBJECT_TYPE_PROCESS;
 p->state=PROCESS_STATE_ACTIVE;p->launch_ready=true;p->image_loaded=true;
 p->address_space=p;p->tls_present=true;p->handle_capacity=8;
 p->handle_table=calloc(p->handle_capacity,sizeof(process_handle_slot_t));
 assert(p->handle_table);p->handle_lock=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
 mutex_init(&p->handle_growth_mutex);
 mutex_init(&p->memory_lock);
 p->main_thread=main_thread;p->threads=main_thread;p->thread_count=1;
 p->live_thread_count=1;main_thread->id=1;main_thread->state=THREAD_STATE_RUNNING;
 main_thread->running_cpu=0;main_thread->process=p;
 main_thread->process_initial=true;main_thread->process_reference_held=true;
 main_thread->process_state=THREAD_PROCESS_ATTACHED;local_thread=main_thread;
 test_process=p;}
static void worker_complete(process_thread_record_t*r,u64 value)
{kernel_thread_t*t=r->execution;process_worker_exit(t,value);
 t->state=THREAD_STATE_TERMINATED;t->running_cpu=THREAD_CPU_NONE;
 process_scheduler_handoff_complete(t);}
static bool fixture_sleep_consumes_event(kernel_thread_t *thread)
{u64 flags=spin_lock_irqsave(&scheduler_lock);
 bool consumed=scheduler_consume_pending_wake_locked(thread,false);
 spin_unlock_irqrestore(&scheduler_lock,flags);return consumed;}
static void clear(process_t*p)
{assert(!p->threads||p->threads==p->main_thread);
 free(p->handle_table);
 pthread_mutex_destroy(&p->handle_growth_mutex.state_lock);
 pthread_mutex_destroy(&p->memory_lock.state_lock);
 pthread_mutex_destroy(&p->handle_lock);}
typedef struct {process_t*p;process_handle_t h;mg_thread_result_t result;
                kernel_thread_t *caller;mg_result_t status;} join_call_t;
typedef struct {process_t*p;process_handle_t h;mg_result_t status;}
    detach_call_t;
typedef struct {process_t*p;mg_thread_created_t made;mg_result_t status;}
    create_call_t;
static void *join_host(void*raw)
{join_call_t*c=raw;local_thread=c->caller?c->caller:test_process->main_thread;
 c->status=process_thread_join(c->p,c->h,&c->result);return NULL;}
static void *create_host(void *raw)
{create_call_t*c=raw;local_thread=test_process->main_thread;
 c->status=process_thread_create(c->p,0x400100,13,&c->made.handle,
                                 &c->made.id);return NULL;}
static void *reap_host(void *raw)
{local_thread=test_process->main_thread;
 process_worker_reclaim_pending((process_t*)raw);return NULL;}
static void *complete_host(void *raw)
{local_thread=test_process->main_thread;
 worker_complete((process_thread_record_t*)raw,0x7788U);return NULL;}
static void *detach_host(void *raw)
{detach_call_t*c=raw;local_thread=test_process->main_thread;
 c->status=process_thread_detach(c->p,c->h);
 pthread_mutex_lock(&event_lock);reap_race_done=true;
 pthread_cond_broadcast(&event_changed);pthread_mutex_unlock(&event_lock);
 return NULL;}
static void *lost_detach_host(void *raw)
{detach_call_t*c=raw;local_thread=test_process->main_thread;
 c->status=process_thread_detach(c->p,c->h);
 pthread_mutex_lock(&event_lock);lost_race_done=true;
 pthread_cond_broadcast(&event_changed);pthread_mutex_unlock(&event_lock);
 return NULL;}
static void test_after_created_handle_is_visible(void)
{
 if(!create_reap_after_publish)return;
 create_reap_after_publish=false;
 process_thread_record_t *record=test_process->thread_records;
 assert(record&&record->execution&&record->execution->queued);
 /* The creator has stored out_handle, so a sibling sharing the output
  * object can consume it before the creator stores out_thread_id. */
 assert(process_thread_detach(test_process,last_reserved_handle)==MG_OK);
 worker_complete(record,0x9911U);
 process_worker_reclaim_pending(test_process);
}
static void run_wake_failure_case(const char *mode)
{
 process_t p;kernel_thread_t main_thread;mg_thread_created_t target;
 process_thread_record_t *target_record;
 init(&p,&main_thread);
 assert(process_thread_create(&p,0x400100,0x51,&target.handle,&target.id)==MG_OK);
 target_record=p.thread_records;
 if(!strcmp(mode,"--bad-join-pair")){
  target_record->join_claimed=true;
 }else if(!strcmp(mode,"--bad-join-owner")){
  static kernel_thread_t foreign;
  memset(&foreign,0,sizeof(foreign));foreign.state=THREAD_STATE_READY;
  target_record->join_claimed=true;target_record->join_waiter=&foreign;
 }else if(!strcmp(mode,"--missing-join-member")){
  static kernel_thread_t foreign;
  memset(&foreign,0,sizeof(foreign));foreign.state=THREAD_STATE_READY;
  foreign.process=&p;foreign.process_reference_held=true;
  foreign.process_state=THREAD_PROCESS_ATTACHED;
  target_record->join_claimed=true;target_record->join_waiter=&foreign;
 }else if(!strcmp(mode,"--live-wake-failure")){
  mg_thread_created_t waiter;
  assert(process_thread_create(&p,0x400100,0x52,&waiter.handle,
                               &waiter.id)==MG_OK);
  kernel_thread_t *waiter_thread=p.thread_records->execution;
  target_record=p.thread_records->next;
  assert(target_record&&target_record->execution);
  target_record->join_claimed=true;
  target_record->join_waiter=waiter_thread;
  waiter_thread->state=THREAD_STATE_BLOCKED;
  waiter_thread->queued=false;
  scheduler_enqueue_failure=true;
 }else{
  panic("unknown negative-control mode");
 }
 worker_complete(target_record,0x5151U);
 panic("expected join wake validation failure did not occur");
}

int main(int argc,char **argv)
{
 process_t p;kernel_thread_t main_thread;mg_thread_created_t made;
 if(argc==2){
  mutex_init(&vmm_operation_mutex);
  run_wake_failure_case(argv[1]);
  return 0;
 }
 u64 result=0;mutex_init(&vmm_operation_mutex);init(&p,&main_thread);
 assert(process_snapshot_reap_flags_locked(&p)==0);
 p.worker_reap_pending=true;p.user_region_reap_pending=true;
 p.worker_reapers=1;
 assert(process_snapshot_reap_flags_locked(&p)==MG_PROCESS_FLAG_REAP_STATE_MASK);
 p.worker_reap_pending=false;p.user_region_reap_pending=false;
 p.worker_reapers=0;
 fixture_eflags=0;
 assert(!process_worker_reclaim_context_valid()&&fixture_eflags==0);
 fixture_eflags=1ULL<<9;
 assert(process_worker_reclaim_context_valid()&&fixture_eflags==(1ULL<<9));
 assert(process_current_thread_id()==1);
 kernel_thread_t wake_case={0};
 wake_case.state=THREAD_STATE_RUNNING;
 assert(scheduler_wake_waiter_result(&wake_case)==SCHEDULER_WAKE_DELIVERED&&
        wake_case.event_wake_pending);
 scheduler_clear_event_wake(&wake_case);
 wake_case.state=THREAD_STATE_READY;
 assert(scheduler_wake_waiter_result(&wake_case)==SCHEDULER_WAKE_DELIVERED&&
        wake_case.event_wake_pending);
 scheduler_clear_event_wake(&wake_case);
 wake_case.state=THREAD_STATE_BLOCKED;
 wake_case.queued=false;scheduler_enqueue_failure=false;
 assert(scheduler_wake_waiter_result(&wake_case)==SCHEDULER_WAKE_DELIVERED&&
        wake_case.state==THREAD_STATE_READY&&wake_case.queued);
 wake_case.state=THREAD_STATE_BLOCKED;wake_case.queued=false;
 scheduler_enqueue_failure=true;
 assert(scheduler_wake_waiter_result(&wake_case)==SCHEDULER_WAKE_FAILED&&
        wake_case.state==THREAD_STATE_BLOCKED&&!wake_case.queued);
 scheduler_enqueue_failure=false;
 wake_case.state=THREAD_STATE_TERMINATED;
 assert(scheduler_wake_waiter_result(&wake_case)==
        SCHEDULER_WAKE_ALREADY_TERMINATED);
 assert(scheduler_wake_waiter_result(NULL)==SCHEDULER_WAKE_FAILED);
 process_user_region_t missing_region={0};
 assert(process_user_region_release(&p,&missing_region)==
        PROCESS_REGION_RELEASE_INVALID);

 /* Reproduce scheduler post-switch ordering with production mutex code. The
  * unlock below assigns handoff_mutex to main_thread and publishes its wake,
  * while waiting_mutex remains set until main_thread resumes mutex_lock().
  * A VMM revoke during this window must report RETRY without taking or
  * changing the different, contended operation mutex. The worker reaper must
  * likewise leave stack/TLS ownership untouched. */
 assert(process_thread_create(&p,0x400100,0x550,&made.handle,&made.id)==MG_OK);
 process_thread_record_t *handoff_record=p.thread_records;
 process_user_region_t *handoff_stack=handoff_record->stack_region;
 process_user_region_t *handoff_tls=handoff_record->tls_region;
 assert(process_thread_detach(&p,made.handle)==MG_OK);
 worker_complete(handoff_record,0x551);
 mutex_t handoff_mutex;kernel_thread_t prior_owner={0},vmm_owner={0};
 mutex_init(&handoff_mutex);
 handoff_mutex.owner=&prior_owner;
 handoff_mutex.wait_head=handoff_mutex.wait_tail=&main_thread;
 main_thread.state=THREAD_STATE_BLOCKED;
 main_thread.waiting_mutex=&handoff_mutex;
 main_thread.mutex_wait_next=NULL;
 local_thread=&prior_owner;
 assert(mutex_unlock(&handoff_mutex));
 assert(handoff_mutex.owner==&main_thread&&
        main_thread.waiting_mutex==&handoff_mutex&&
        main_thread.mutex_wake_pending);
 main_thread.state=THREAD_STATE_RUNNING;
 local_thread=&main_thread;
 vmm_operation_owner=&vmm_owner;vmm_operation_contended=true;
 assert(vmm_user_range_revoke(p.address_space,
            (void *)handoff_tls->mapping_base,handoff_tls->mapping_pages)==
        VMM_USER_RANGE_REVOKE_RETRY);
 u32 handoff_releases_before=region_releases;
 u32 handoff_refs=handoff_record->object.ref_count;
 for(unsigned attempt=0;attempt<2;attempt++) {
  process_worker_reclaim_pending(&p);
  assert(handoff_record->stack_region==handoff_stack&&
         handoff_record->tls_region==handoff_tls&&
         !handoff_stack->revoked&&!handoff_tls->revoked&&
         !handoff_stack->release_pending&&!handoff_tls->release_pending&&
         !handoff_record->reclaiming&&!p.worker_reapers&&
         handoff_record->object.ref_count==handoff_refs&&
         p.worker_reap_pending&&!p.user_region_reap_pending&&
         region_releases==handoff_releases_before);
 }
 /* The incoming continuation consumes the handoff marker. At the ordinary
  * safe retry point, make the contended VMM lock refuse parking once. The
  * release path must defer without losing region or thread ownership. */
 assert(mutex_lock(&handoff_mutex)&&!main_thread.waiting_mutex&&
        !main_thread.mutex_wake_pending);
 assert(mutex_unlock(&handoff_mutex));
 local_thread=&main_thread;
 refuse_scheduler_block=true;
 process_worker_reclaim_pending(&p);
 refuse_scheduler_block=false;
 assert(handoff_record->stack_region==handoff_stack&&
        handoff_record->tls_region==handoff_tls&&
        !handoff_stack->revoked&&!handoff_tls->revoked&&
        handoff_stack->release_pending&&handoff_tls->release_pending&&
        !handoff_record->reclaiming&&!p.worker_reapers&&
        handoff_record->object.ref_count==handoff_refs&&
        p.worker_reap_pending&&p.user_region_reap_pending&&
        region_releases==handoff_releases_before);
 /* The VMM owner releases the operation lock. The ordinary syscall retry
  * drains the retained stack/TLS regions without another worker exit. */
 local_thread=&vmm_owner;
 assert(mutex_unlock(&vmm_operation_mutex));
 vmm_operation_owner=NULL;vmm_operation_contended=false;
 local_thread=&main_thread;
 process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&p.thread_count==1&&p.live_thread_count==1&&
        !p.worker_reap_pending&&!p.user_region_reap_pending&&
        region_releases==handoff_releases_before+2U&&
        stack_releases>0);
 pthread_mutex_destroy(&handoff_mutex.state_lock);

 fail_thread_create=true;
 assert(process_thread_create(&p,0x400100,7,&made.handle,&made.id)==MG_ERR_NO_MEMORY);
 fail_thread_create=false;
 fail_allocation_at=allocation_count+1;
 assert(process_thread_create(&p,0x400100,7,&made.handle,&made.id)==MG_ERR_NO_MEMORY);
 fail_allocation_at=0;
 fail_prepare=true;
 assert(process_thread_create(&p,0x400100,7,&made.handle,&made.id)==MG_ERR_NO_MEMORY);
 fail_prepare=false;fail_reserve=true;
 assert(process_thread_create(&p,0x400100,7,&made.handle,&made.id)==MG_ERR_NO_MEMORY);
 fail_reserve=false;fail_enqueue=true;
 u32 before_regions=(u32)region_releases;
 assert(process_thread_create(&p,0x400100,7,&made.handle,&made.id)==MG_ERR_BUSY);
 fail_enqueue=false;
 assert(p.thread_count==1&&p.live_thread_count==1&&!p.thread_records&&
        !p.user_regions&&region_releases==before_regions+2&&
        handle_cancels==1);

 /* Fail worker publication while a sibling holds a lease on its provisional
  * stack. Rollback must retain the region in the process reservation list,
  * return a normal creation error, then reclaim it after the lease drains. */
 lease_on_revoke_kind=PROCESS_USER_REGION_WORKER_STACK;
 fail_enqueue=true;
 assert(process_thread_create(&p,0x400100,0x708,&made.handle,&made.id)==
        MG_ERR_BUSY);
 fail_enqueue=false;
 assert(!lease_on_revoke_kind&&leased_region&&p.user_regions==leased_region&&
        p.user_regions->release_pending&&p.user_region_reap_pending&&
        !p.thread_records&&p.thread_count==1&&p.live_thread_count==1);
 leased_region=NULL;
 process_worker_reclaim_pending(&p);
 assert(!p.user_regions&&!p.user_region_reap_pending&&
        p.thread_count==1&&!p.thread_records);

 /* Deterministically consume and reap a newly published worker after its
  * handle is stored to shared output, but before the creator stores its ID.
  * The ID must have been captured before publication. */
 u64 expected_published_id=next_thread_id+1U;
 create_reap_after_publish=true;
 assert(process_thread_create(&p,0x400100,0x9911,&made.handle,&made.id)==MG_OK);
 assert(!create_reap_after_publish&&made.handle&&
        made.id==expected_published_id&&!p.thread_records&&
        p.thread_count==1&&p.live_thread_count==1&&!p.worker_reap_pending);

 unsigned destroyed_before_success=destroyed_threads;
 unsigned stacks_before_success=stack_releases;
 unsigned regions_before_success=region_releases;
 assert(process_thread_create(&p,0x400100,0xabc,&made.handle,&made.id)==MG_OK);
 assert(made.id>1&&made.handle&&p.thread_count==2&&p.live_thread_count==2&&
        p.thread_records&&p.thread_records->execution&&
        p.thread_records->execution->worker_record==p.thread_records&&
        p.thread_records->object.ref_count==2);
 process_thread_record_t*r=p.thread_records;
 assert(!process_handle_close(&p,made.handle)); /* still joinable */
 assert(object_reference(&r->object)); /* an in-flight operation pins it */
 worker_complete(r,0x123456789abcdef0ULL);
 assert(r->completed&&r->result==0x123456789abcdef0ULL&&
        p.live_thread_count==1);
 leased_region=r->stack_region;r->stack_region->leases=1;
 process_worker_reclaim_pending(&p);
 assert(r->kernel_stack_released&&r->execution==NULL&&
        r->tls_region==NULL&&r->stack_region==leased_region&&
        p.thread_count==1&&destroyed_threads==destroyed_before_success+1);
 leased_region=NULL;r->stack_region->leases=0;
 process_worker_reclaim_pending(&p);
 assert(r->stack_region==NULL&&r->execution==NULL&&
        p.thread_records==r&&p.thread_count==1);
 assert(!p.worker_reap_pending); /* joinable record needs no resource retry */
 /* This first operation is actually a join, so completion authority remains
  * valid after the heavy execution context was reclaimed. */
 assert(process_thread_join(&p,made.handle,&result)==MG_OK&&
        result==0x123456789abcdef0ULL);
 assert(process_thread_join(&p,made.handle,&result)==MG_ERR_INVALID_HANDLE);
 process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&p.thread_count==1&&p.live_thread_count==1&&
        p.threads==&main_thread&&destroyed_threads==destroyed_before_success+1);
 assert(stack_releases==stacks_before_success+1&&
        region_releases==regions_before_success+2&&!p.worker_reap_pending);
 object_release(&r->object); /* operation reference retained across cleanup */

 /* Reusing the released slot advances its generation. The previous handle
  * must remain invalid while the new worker retains join authority. */
 process_handle_t stale_handle=made.handle;
 assert(process_thread_create(&p,0x400100,8,&made.handle,&made.id)==MG_OK);
 assert(made.handle!=stale_handle &&
        (made.handle&0xffffU)==(stale_handle&0xffffU));
 assert(process_thread_join(&p,stale_handle,&result)==MG_ERR_INVALID_HANDLE);
 r=p.thread_records;worker_complete(r,89);
 assert(process_thread_join(&p,made.handle,&result)==MG_OK&&result==89);
 process_worker_reclaim_pending(&p);assert(!p.thread_records&&p.thread_count==1);

 /* A completed worker keeps only its completion object until detach; the
  * scheduler context is already gone and the thread no longer counts as an
  * attached process execution thread. */
 assert(process_thread_create(&p,0x400100,81,&made.handle,&made.id)==MG_OK);
 r=p.thread_records;worker_complete(r,91);process_worker_reclaim_pending(&p);
 assert(r->execution==NULL&&r->listed&&p.thread_count==1&&
        r->kernel_stack_released&&!r->stack_region&&!r->tls_region);
 assert(process_thread_detach(&p,made.handle)==MG_OK);
 process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&p.thread_count==1&&!p.worker_reap_pending);

 /* Exactly one joiner claims a worker. The second returns busy while the
  * winner sleeps, and completion wakeup cannot be lost before scheduler_block. */
 assert(process_thread_create(&p,0x400100,9,&made.handle,&made.id)==MG_OK);
 r=p.thread_records;
 join_call_t a={.p=&p,.h=made.handle},b={.p=&p,.h=made.handle};
 pthread_t ta,tb;assert(pthread_create(&ta,NULL,join_host,&a)==0);
 pthread_mutex_lock(&event_lock);
 while(!block_entered)pthread_cond_wait(&event_changed,&event_lock);
 block_entered=false;
 pthread_mutex_unlock(&event_lock);
 assert(process_thread_detach(&p,made.handle)==MG_ERR_BUSY);
 assert(!process_handle_close(&p,made.handle));
 assert(pthread_create(&tb,NULL,join_host,&b)==0);
 assert(pthread_join(tb,NULL)==0&&b.status==MG_ERR_BUSY);
 worker_complete(r,77);
 assert(pthread_join(ta,NULL)==0&&a.status==MG_OK&&a.result==77);
 process_worker_reclaim_pending(&p);assert(!p.thread_records&&p.thread_count==1);

 /* An interrupted join releases its sole-join claim without consuming the
  * handle, so a later join can still collect the worker result. */
 assert(process_thread_create(&p,0x400100,10,&made.handle,&made.id)==MG_OK);
 r=p.thread_records;
 join_call_t interrupted={.p=&p,.h=made.handle};
 assert(pthread_create(&ta,NULL,join_host,&interrupted)==0);
 pthread_mutex_lock(&event_lock);
 while(!block_entered)pthread_cond_wait(&event_changed,&event_lock);
 block_entered=false;
 pthread_mutex_unlock(&event_lock);
 __atomic_store_n(&interrupt_pending,true,__ATOMIC_RELEASE);
 assert(scheduler_wake_waiter(&main_thread));
 assert(pthread_join(ta,NULL)==0&&interrupted.status==MG_ERR_INTERRUPTED);
 assert(!r->join_claimed&&!r->joined&&!r->detached);
 worker_complete(r,88);
 assert(process_thread_join(&p,made.handle,&result)==MG_OK&&result==88);
 process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&p.thread_count==1);

 /* A distinct detached worker joins another worker. Pause after the
  * production scheduler has published the completion wake but before its
  * return to the registry-locked completion callback. The joiner can run, but
  * cannot cancel or consume the registration until wake publication ends. */
 process_handle_t target_handle;
 assert(process_thread_create(&p,0x400100,0x21,&target_handle,&made.id)==MG_OK);
 process_thread_record_t *target_record=p.thread_records;
 process_handle_t joining_handle;
 assert(process_thread_create(&p,0x400100,0x22,&joining_handle,&made.id)==MG_OK);
 process_thread_record_t *joining_record=p.thread_records;
 assert(joining_record!=target_record&&joining_record->execution);
 assert(object_reference(&joining_record->object));
 kernel_thread_t *joining_thread=joining_record->execution;
 assert(process_thread_detach(&p,joining_handle)==MG_OK);
 join_call_t detached_join={.p=&p,.h=target_handle,
                            .caller=joining_thread};
 block_entered=false;
 assert(pthread_create(&ta,NULL,join_host,&detached_join)==0);
 pthread_mutex_lock(&event_lock);
 while(!block_entered)pthread_cond_wait(&event_changed,&event_lock);
 block_entered=false;
 pthread_mutex_unlock(&event_lock);
 wake_delivery_pause_armed=true;wake_delivery_paused=false;
 wake_delivery_resume=false;registry_attempt_watch=joining_thread;
 registry_attempt_seen=false;
 assert(pthread_create(&tb,NULL,complete_host,target_record)==0);
 pthread_mutex_lock(&event_lock);
 while(!wake_delivery_paused)pthread_cond_wait(&event_changed,&event_lock);
 pthread_mutex_unlock(&event_lock);
 assert(target_record->completed&&
        target_record->join_waiter==joining_thread);
 pthread_mutex_lock(&event_lock);
 while(!registry_attempt_seen)pthread_cond_wait(&event_changed,&event_lock);
 pthread_mutex_unlock(&event_lock);
 assert(pthread_mutex_trylock(&process_registry_lock)==EBUSY);
 assert(detached_join.status==0&&target_record->join_claimed&&
        target_record->join_waiter==joining_thread);
 pthread_mutex_lock(&event_lock);wake_delivery_resume=true;
 pthread_cond_broadcast(&event_changed);pthread_mutex_unlock(&event_lock);
 assert(pthread_join(tb,NULL)==0);
 assert(pthread_join(ta,NULL)==0&&detached_join.status==MG_OK&&
        detached_join.result==0x7788U);
 registry_attempt_watch=NULL;
 assert(!target_record->join_claimed&&!target_record->join_waiter&&
        target_record->joined&&!joining_thread->event_wake_pending);
 worker_complete(joining_record,0x2233U);
 process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&p.thread_count==1);
 assert(joining_record->object.ref_count==1);
 object_release(&joining_record->object); /* fixture-only retained pin */

 /* A waiter that is already a terminated, still process-owned descriptor is
  * a canceled registration. Completion retains the result and makes it
  * available to the initial thread; it does not make the waiter runnable. */
 assert(process_thread_create(&p,0x400100,0x31,&target_handle,&made.id)==MG_OK);
 target_record=p.thread_records;
 assert(process_thread_create(&p,0x400100,0x32,&joining_handle,&made.id)==MG_OK);
 joining_record=p.thread_records;joining_thread=joining_record->execution;
 assert(process_thread_detach(&p,joining_handle)==MG_OK);
 u64 join_flags=spin_lock_irqsave(&process_registry_lock);
 target_record->join_claimed=true;target_record->join_waiter=joining_thread;
 spin_unlock_irqrestore(&process_registry_lock,join_flags);
 worker_complete(joining_record,0x3232U);
 assert(joining_thread->state==THREAD_STATE_TERMINATED&&
        joining_thread->process_state==THREAD_PROCESS_HANDOFF_COMPLETE);
 worker_complete(target_record,0x31U);
 assert(target_record->completed&&!target_record->join_claimed&&
        !target_record->join_waiter);
 assert(process_thread_join(&p,target_handle,&result)==MG_OK&&result==0x31U);
 process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&p.thread_count==1);

 /* Cancellation clears the registration and event latch before the same
  * thread starts a later timed sleep. Delayed target completion has no waiter
  * pointer to wake, so it cannot consume or shorten that sleep. */
 assert(process_thread_create(&p,0x400100,0x41,&made.handle,&made.id)==MG_OK);
 r=p.thread_records;
 join_call_t canceled_join={.p=&p,.h=made.handle,
                             .caller=&main_thread,.status=1};
 block_entered=false;
 assert(pthread_create(&ta,NULL,join_host,&canceled_join)==0);
 pthread_mutex_lock(&event_lock);
 while(!block_entered)pthread_cond_wait(&event_changed,&event_lock);
 block_entered=false;
 pthread_mutex_unlock(&event_lock);
 __atomic_store_n(&interrupt_pending,true,__ATOMIC_RELEASE);
 assert(scheduler_wake_waiter(&main_thread));
 assert(pthread_join(ta,NULL)==0&&
        canceled_join.status==MG_ERR_INTERRUPTED);
 assert(!r->join_claimed&&!r->join_waiter&&
        !main_thread.event_wake_pending);
 main_thread.state=THREAD_STATE_BLOCKED;main_thread.sleeping=true;
 worker_complete(r,0x41U);
 assert(main_thread.state==THREAD_STATE_BLOCKED&&main_thread.sleeping&&
        !main_thread.event_wake_pending&&
        !fixture_sleep_consumes_event(&main_thread));
 main_thread.state=THREAD_STATE_RUNNING;main_thread.sleeping=false;
 assert(process_thread_join(&p,made.handle,&result)==MG_OK&&result==0x41U);
 process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&p.thread_count==1);

 /* Detachment closes the handle but retains the process-owned record until
  * final scheduler handoff; completion then frees stack, TLS and record. */
 unsigned destroyed_before_detach=destroyed_threads;
 unsigned stacks_before_detach=stack_releases;
 unsigned regions_before_detach=region_releases;
 assert(process_thread_create(&p,0x400100,11,&made.handle,&made.id)==MG_OK);
 r=p.thread_records;
 assert(process_thread_detach(&p,made.handle)==MG_OK&&r->detached);
 assert(process_thread_detach(&p,made.handle)==MG_ERR_INVALID_HANDLE);
 worker_complete(r,0x55);process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&p.thread_count==1&&p.live_thread_count==1&&
        destroyed_threads==destroyed_before_detach+1&&
        stack_releases==stacks_before_detach+1&&
        region_releases==regions_before_detach+2&&!p.worker_reap_pending);

 /* Force detach to commit after execution resources are released but before
  * the reaper retires the join record. One of the two owners must consume the
  * handle entitlement without stranding execution or record ownership. */
 assert(process_thread_create(&p,0x400100,14,&made.handle,&made.id)==MG_OK);
 r=p.thread_records;worker_complete(r,0x77);
 reap_race_armed=true;reap_race_paused=false;reap_race_done=false;
 detach_call_t raced_detach={.p=&p,.h=made.handle};
 pthread_t reaper,detacher;
 assert(pthread_create(&reaper,NULL,reap_host,&p)==0);
 pthread_mutex_lock(&event_lock);
 while(!reap_race_paused)pthread_cond_wait(&event_changed,&event_lock);
 pthread_mutex_unlock(&event_lock);
 assert(pthread_create(&detacher,NULL,detach_host,&raced_detach)==0);
 assert(pthread_join(detacher,NULL)==0&&raced_detach.status==MG_OK);
 assert(pthread_join(reaper,NULL)==0);
 assert(!p.thread_records&&p.thread_count==1&&p.live_thread_count==1&&
        !p.worker_reap_pending);

 /* Consume a join entitlement after the per-record reaper scan has decided
  * to retain a completed joinable record, but before it recomputes the work
  * hint. That final hint must still request another reap pass. */
 assert(process_thread_create(&p,0x400100,0x141,&made.handle,&made.id)==MG_OK);
 r=p.thread_records;worker_complete(r,0x141);
 lost_race_armed=true;lost_race_paused=false;lost_race_done=false;
 detach_call_t lost_detach={.p=&p,.h=made.handle};
 assert(pthread_create(&reaper,NULL,reap_host,&p)==0);
 pthread_mutex_lock(&event_lock);
 while(!lost_race_paused)pthread_cond_wait(&event_changed,&event_lock);
 pthread_mutex_unlock(&event_lock);
 assert(pthread_create(&detacher,NULL,lost_detach_host,&lost_detach)==0);
 assert(pthread_join(detacher,NULL)==0&&lost_detach.status==MG_OK);
 assert(pthread_join(reaper,NULL)==0&&p.thread_records==r&&
        r->detached&&p.worker_reap_pending);
 process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&p.thread_count==1&&!p.worker_reap_pending);

 /* If join consumes the right while another thread still holds a user-stack
  * lease, the execution object is reaped first and the completion record must
  * be removed after the later mapping-unmap retry succeeds. */
 assert(process_thread_create(&p,0x400100,12,&made.handle,&made.id)==MG_OK);
 r=p.thread_records;assert(object_reference(&r->object));
 worker_complete(r,0x66);
 leased_region=r->stack_region;r->stack_region->leases=1;
 assert(process_thread_join(&p,made.handle,&result)==MG_OK&&result==0x66);
 process_worker_reclaim_pending(&p);
 assert(!r->execution&&r->stack_region==leased_region&&
        r->kernel_stack_released&&p.thread_count==1&&p.thread_records==r&&
        p.worker_reap_pending);
 leased_region=NULL;r->stack_region->leases=0;
 process_worker_reclaim_pending(&p);
 assert(!p.thread_records&&!p.worker_reap_pending&&p.thread_count==1&&
        p.threads==&main_thread&&r->stack_region==NULL&&r->tls_region==NULL);
 object_release(&r->object);

 /* Admission closes while private stack/TLS resources are being prepared.
  * The constructor rolls back and never publishes a runnable thread. */
 pause_prepare=true;prepare_entered=false;allow_prepare=false;
 create_call_t cc={.p=&p};
 pthread_t tc;assert(pthread_create(&tc,NULL,create_host,&cc)==0);
 pthread_mutex_lock(&event_lock);
 while(!prepare_entered)pthread_cond_wait(&event_changed,&event_lock);
 pthread_mutex_unlock(&event_lock);
 u64 f=spin_lock_irqsave(&process_registry_lock);
 p.state=PROCESS_STATE_EXITING;p.exit_requested=true;
 spin_unlock_irqrestore(&process_registry_lock,f);
 pthread_mutex_lock(&event_lock);allow_prepare=true;
 pthread_cond_broadcast(&event_changed);pthread_mutex_unlock(&event_lock);
 assert(pthread_join(tc,NULL)==0&&cc.status==MG_ERR_SHUTDOWN);
 assert(p.thread_count==1&&p.live_thread_count==1&&!p.thread_records&&
        !p.user_regions);
 clear(&p);
 pthread_mutex_destroy(&vmm_operation_mutex.state_lock);
 puts("thread API: transactional creation, serialized join completion/cancellation, wake outcomes, detach, handoff- and lease-deferred stack/TLS reap, and exit admission passed");
 return 0;
}
'''

functions = []
for source_path, name in names:
    source = function(source_path, name)
    if name == "process_thread_create":
        old = ("if (published) {\n        *out_handle = handle;\n"
               "        *out_thread_id = thread_id;")
        new = ("if (published) {\n        *out_handle = handle;\n"
               "        test_after_created_handle_is_visible();\n"
               "        *out_thread_id = thread_id;")
        if old not in source:
            raise AssertionError("thread-create publication hook point changed")
        source = source.replace(old, new, 1)
    functions.append(source)
mutex_source = (ROOT / "kernel/src/mutex.c").read_text()
mutex_source = re.sub(r"^#include .*\n", "", mutex_source, flags=re.M)
mutex_source = re.sub(
    r"static u64 mutex_irq_save\(void\).*?\n}",
    "static u64 mutex_irq_save(void) {return 0;}", mutex_source,
    count=1, flags=re.S)
mutex_source = re.sub(
    r"static void mutex_irq_restore\(u64 flags\).*?\n}",
    "static void mutex_irq_restore(u64 flags) {(void)flags;}", mutex_source,
    count=1, flags=re.S)
code = prefix + mutex_source + "\n".join(functions) + suffix
with HostFixture("thread-api") as fixture:
    directory = fixture.path
    path = Path(directory) / "process_thread_api.c"
    binary = Path(directory) / "process_thread_api"
    path.write_text('#include "' + str(ROOT / "tests/kernel/process_cleanup_fixture.h") + '"\n' + code)
    fixture.run([CC, "-std=gnu17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-pthread", str(path),
                    "-o", str(binary)], check=True)
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    print(fixture.run([str(binary)], env=env).stdout, end="")

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    for mode, expected in (
        ("--bad-join-pair", "inconsistent worker join registration"),
        ("--bad-join-owner", "worker joiner lost process ownership"),
        ("--missing-join-member", "worker joiner lost process ownership"),
        ("--live-wake-failure", "failed to wake completed worker joiner"),
    ):
        failure = fixture.run([str(binary), mode], env=env, check=False)
        if failure.returncode == 0 or expected not in failure.stderr:
            raise AssertionError(
                f"{mode} did not fail at {expected!r}: "
                f"status={failure.returncode}, stderr={failure.stderr!r}")
        print(f"negative control {mode}: rejected ({failure.returncode})")
