#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production scheduler-reference transfer and cleanup-worker drain."""
from pathlib import Path
import ast
import os
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/shared"))
from host_fixture import HostFixture
CC = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('HOST_CC', 'gcc')


def c_function(source, name):
    pattern = r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\('
    for match in re.finditer(pattern, source, re.M):
        brace = source.index('{', match.end())
        if ';' in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end] + '\n'
    raise ValueError(name)


def fixture_prefix():
    test_source = (ROOT / 'tests/kernel/process_reap_test.py').read_text()
    module = ast.parse(test_source)
    for node in module.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == 'prefix'
                for target in node.targets):
            prefix = ast.literal_eval(node.value)
            break
    else:
        raise ValueError('process_reap_test.py prefix')

    prefix = prefix.replace(
        '/* This fixture models a safe process context and does not include the\n'
        ' * deferred cleanup worker. The focused deferred-release fixture exercises\n'
        ' * the production unsafe-context retry path. */\n'
        'static bool process_cleanup_context_valid(void){return true;}\n'
        'static void process_cleanup_worker_request_session_collection_retry(void){}\n',
        '')

    prefix = prefix.replace('#include <setjmp.h>',
                            '#include <setjmp.h>\n#include <pthread.h>\n'
                            '#include <stdatomic.h>\n#include <sched.h>')
    prefix = prefix.replace('#define PROCESS_NO_SESSION 0',
                            '#define PROCESS_NO_SESSION 0\n'
                            '#define PROCESS_HANDOFF_RELEASE_BATCH 16U\n'
                            '#define PROCESS_DEFERRED_REFERENCE_RELEASE_BATCH 16U')
    prefix = prefix.replace(
        'enum {THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,\n'
        '      THREAD_STATE_TERMINATED};',
        'enum {THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,\n'
        '      THREAD_STATE_TERMINATED};\n'
        'typedef int thread_state_t;')
    prefix = prefix.replace(
        'static process_t parent, child, *all_processes;',
        'static process_t parent, child, *all_processes;\n'
        'static process_handle_slot_t pid_handle_slot;\n'
        'static _Thread_local bool in_scheduler_callback, incoming_mutex_pending;\n'
        'static _Thread_local bool fixture_interrupts_enabled, in_cleanup_worker;\n'
        'static _Atomic unsigned wake_count, publishers_done;\n'
        'static pthread_mutex_t wake_gate_lock=PTHREAD_MUTEX_INITIALIZER;\n'
        'static pthread_cond_t wake_gate_changed=PTHREAD_COND_INITIALIZER;\n'
        'static bool wake_entered, release_paused_wake;\n'
        'static _Thread_local bool publisher_context, pause_this_wake;\n'
        'static kernel_thread_t cleanup_worker_sentinel;\n'
        'static kernel_thread_t *process_cleanup_worker=\n'
        '    &cleanup_worker_sentinel;\n'
        'static _Atomic bool process_session_collection_retry_pending;\n'
        'static _Atomic bool process_cleanup_worker_ready=true;\n'
        'static _Atomic unsigned collection_retry_wake_count;\n'
        'static u32 session_fixture_lock;\n'
        'static bool helper_snapshot_paused, termination_published;\n'
        'static bool retry_scan_paused, retry_published;\n'
        'static bool pause_pid_lookup, pid_lookup_pinned, pid_close_done;\n'
        'static pthread_mutex_t pid_gate_lock=PTHREAD_MUTEX_INITIALIZER;\n'
        'static pthread_cond_t pid_gate_changed=PTHREAD_COND_INITIALIZER;\n'
        'static u32 process_destructions;\n'
        'static u32 unsafe_destructor_entries;\n'
        'static _Thread_local unsigned fixture_lock_depth;\n'
        'static void object_release(kernel_object_t *);\n'
        'static bool process_finish_termination(process_t *);\n'
        'static void process_object_destroy(kernel_object_t *);\n'
        'static void process_cleanup_worker_entry(void *unused)'
        '{(void)unused;}\n'
        'static kernel_thread_t *thread_create_suspended(const char *name, '
        'void (*entry)(void *), void *arg)\n'
        '{(void)name;(void)entry;(void)arg;return &cleanup_worker_sentinel;}\n'
        'static bool scheduler_enqueue(kernel_thread_t *thread)'
        '{return thread==&cleanup_worker_sentinel;}\n'
        'static bool scheduler_terminate_thread(kernel_thread_t *thread)'
        '{return thread==&cleanup_worker_sentinel;}\n'
        'static bool process_thread_handle_close_allowed(kernel_object_t *o)'
        '{(void)o;return true;}\n'
        'static void process_reap_session_members(mg_session_id_t);\n'
        'static void process_collect_completed(mg_session_id_t,bool);')
    prefix = prefix.replace('i32 exit_status; u64 session_id; u32 active_syscalls;',
                            'i32 exit_status; u64 pid; u64 session_id; '
                            'u32 active_syscalls;')
    prefix = prefix.replace('typedef u64 process_handle_t;',
                            'typedef u64 process_handle_t;\n'
                            'typedef size_t usize;\n'
                            'typedef uint16_t u16;\n'
                            'typedef int kernel_object_type_t;\n'
                            '#define HANDLE_INDEX_MASK 0xffffU')
    prefix = prefix.replace(
        'enum {PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING, PROCESS_STATE_TERMINATED};',
        'enum {PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING, PROCESS_STATE_TERMINATED};\n'
        'enum {OBJECT_TYPE_INVALID, OBJECT_TYPE_THREAD=2, '
        'OBJECT_RIGHT_READ=1, '
        'OBJECT_RIGHT_WRITE=2};')
    prefix = prefix.replace('bool context_switch_pending, syscall_active, sleeping;',
                            'bool context_switch_pending, syscall_active, sleeping;\n'
                            ' bool waiting_mutex;')
    prefix = prefix.replace(
        'typedef struct {unsigned ref_count; process_t *process; int type;}\n'
        ' kernel_object_t;',
        'typedef struct kernel_object {unsigned ref_count; process_t *process; '
        'int type; void (*destroy)(struct kernel_object *); } kernel_object_t;')
    prefix = prefix.replace(
        'typedef struct kernel_object {unsigned ref_count; process_t *process; '
        'int type; void (*destroy)(struct kernel_object *); } kernel_object_t;',
        'typedef struct kernel_object {unsigned ref_count; process_t *process; '
        'int type; void (*destroy)(struct kernel_object *); } kernel_object_t;\n'
        'typedef struct {kernel_object_t *object; u32 rights; u16 generation; '
        'bool active, reserved;} process_handle_slot_t;')
    prefix = prefix.replace(
        'kernel_object_t object;\n void *address_space, *handle_table;',
        'kernel_object_t object;\n void *address_space, *handle_table; '
        'usize handle_capacity, next_handle_slot; u32 handle_lock; '
        'pthread_mutex_t handle_growth_mutex; u64 handle_revision;')
    prefix = prefix.replace(
        'static u32 process_registry_lock, scheduler_lock;',
        'static u32 process_registry_lock, scheduler_lock;\n'
        'static pthread_mutex_t registry_mutex=PTHREAD_MUTEX_INITIALIZER;\n'
        'static pthread_mutex_t scheduler_mutex=PTHREAD_MUTEX_INITIALIZER;\n'
        'static pthread_mutex_t session_mutex=PTHREAD_MUTEX_INITIALIZER;\n'
        'static pthread_mutex_t handle_mutex=PTHREAD_MUTEX_INITIALIZER;\n'
        'static pthread_mutex_t collection_gate=PTHREAD_MUTEX_INITIALIZER;\n'
        'static pthread_cond_t collection_changed=PTHREAD_COND_INITIALIZER;\n'
        'static _Thread_local unsigned collection_role;\n'
        'static bool collector_selected, worker_pin_paused, collector_done;')
    prefix = prefix.replace(
        'static u64 spin_lock_irqsave(u32 *lock)\n'
        '{assert(!*lock);*lock=1;return 1;}\n'
        'static void spin_unlock_irqrestore(u32 *lock,u64 flags)\n'
        '{(void)flags;assert(*lock);*lock=0;}',
        'static pthread_mutex_t *fixture_mutex(u32 *lock)\n'
        '{return lock==&process_registry_lock?&registry_mutex:\n'
        ' lock==&session_fixture_lock?&session_mutex:\n'
        ' lock==&parent.handle_lock?&handle_mutex:&scheduler_mutex;}\n'
        'static u64 spin_lock_irqsave(u32 *lock)\n'
        '{u64 saved=fixture_interrupts_enabled?1U:0U;'
        'assert(!pthread_mutex_lock(fixture_mutex(lock)));assert(!*lock);'
        '*lock=1;fixture_interrupts_enabled=false;fixture_lock_depth++;'
        'return saved;}\n'
        'static void spin_unlock_irqrestore(u32 *lock,u64 flags)\n'
        '{(void)flags;assert(*lock);*lock=0;'
        'assert(fixture_lock_depth);fixture_lock_depth--;'
        'fixture_interrupts_enabled=flags!=0;'
        'assert(!pthread_mutex_unlock(fixture_mutex(lock)));\n'
        'if(lock!=&process_registry_lock)return;\n'
        'assert(!pthread_mutex_lock(&collection_gate));\n'
        'if(collection_role==1 && !collector_selected){\n'
        ' assert(child.object.ref_count==4);collector_selected=true;'
        'pthread_cond_broadcast(&collection_changed);\n'
        ' while(!worker_pin_paused)'
        'pthread_cond_wait(&collection_changed,&collection_gate);\n'
        '}else if(collection_role==2 && !worker_pin_paused){\n'
        ' assert(child.scheduler_handoff_references==0 && '
        'child.object.ref_count==4);worker_pin_paused=true;'
        'pthread_cond_broadcast(&collection_changed);\n'
        ' while(!collector_done)'
        'pthread_cond_wait(&collection_changed,&collection_gate);\n'
        '}else if(collection_role==3 && !helper_snapshot_paused){'
        'assert(child.state==PROCESS_STATE_EXITING && '
        'child.object.ref_count==3);helper_snapshot_paused=true;'
        'pthread_cond_broadcast(&collection_changed);'
        'while(!termination_published)'
        'pthread_cond_wait(&collection_changed,&collection_gate);'
        '}else if(collection_role==7 && !retry_scan_paused){'
        'retry_scan_paused=true;pthread_cond_broadcast(&collection_changed);'
        'while(!retry_published)'
        'pthread_cond_wait(&collection_changed,&collection_gate);'
        '}\n'
        'assert(!pthread_mutex_unlock(&collection_gate));}')
    prefix += ('static bool mutex_lock(pthread_mutex_t *mutex)\n'
               '{return pthread_mutex_lock(mutex)==0;}\n'
               'static bool mutex_unlock(pthread_mutex_t *mutex)\n'
               '{return pthread_mutex_unlock(mutex)==0;}\n')
    prefix = prefix.replace(
        'static void spin_lock(u32 *lock){(void)spin_lock_irqsave(lock);}\n'
        'static void spin_unlock(u32 *lock){spin_unlock_irqrestore(lock,0);}',
        'static void spin_lock(u32 *lock)\n'
        '{assert(!pthread_mutex_lock(fixture_mutex(lock)));assert(!*lock);'
        '*lock=1;fixture_lock_depth++;}\n'
        'static void spin_unlock(u32 *lock)\n'
        '{assert(*lock);*lock=0;assert(fixture_lock_depth);'
        'fixture_lock_depth--;'
        'assert(!pthread_mutex_unlock(fixture_mutex(lock)));}')
    # Use the real atomic object reference implementation below.
    prefix = prefix.replace(c_function(prefix, 'object_reference'), '')
    prefix = prefix.replace(c_function(prefix, 'object_release'), '')
    prefix = prefix.replace(
        c_function(prefix, 'process_handle_lookup_any'),
        'static bool object_reference(kernel_object_t *object);\n'
        'static kernel_object_t *process_handle_lookup_any(process_t *p,\n'
        ' process_handle_t h,int type,u32 rights)\n'
        '{(void)rights;assert(p==&parent && (h==7U || h==0x10001U) && '
        'type==OBJECT_TYPE_PROCESS);\n'
        ' assert(object_reference(&child.object));\n'
        ' if(pause_pid_lookup && collection_role==6){'
        'pthread_mutex_lock(&pid_gate_lock);pid_lookup_pinned=true;'
        'pthread_cond_broadcast(&pid_gate_changed);'
        'while(!pid_close_done)'
        'pthread_cond_wait(&pid_gate_changed,&pid_gate_lock);'
        'pthread_mutex_unlock(&pid_gate_lock);}\n'
        ' return &child.object;}\n')
    prefix = prefix.replace(
        'static bool kernel_stack_release_context_valid(void){return true;}',
        'static u32 spinlock_depth_current(void){return fixture_lock_depth;}\n'
        'static bool kernel_stack_release_context_valid(void)\n'
        '{return !spinlock_depth_current() && fixture_interrupts_enabled;}')
    prefix = prefix.replace(
        'static void process_cwd_clear(process_t *p){assert(p==&child);}',
        'static void process_cwd_clear(process_t *p){assert(p==&child); '
        'if(!fixture_interrupts_enabled || fixture_lock_depth || '
        'incoming_mutex_pending || in_scheduler_callback)'
        'unsafe_destructor_entries++; '
        'assert(fixture_interrupts_enabled && !fixture_lock_depth && '
        '!incoming_mutex_pending && !in_scheduler_callback);}')
    prefix = prefix.replace('static void kfree(void *p){(void)p;frees++;}',
                            'static void kfree(void *p){if(p==&child)'
                            'process_destructions++;frees++;}')
    prefix = prefix.replace(
        'static void process_unlink_locked(process_t *p)\n'
        '{assert(process_registry_lock && p==&child);p->parent=NULL;}',
        'static void process_unlink_locked(process_t *p)\n'
        '{assert(process_registry_lock && p==&child);\n'
        ' if(p->parent){process_t **c=&p->parent->first_child;'
        'while(*c && *c!=p)c=&(*c)->next_sibling;'
        'if(*c==p)*c=p->next_sibling;}\n'
        ' p->parent=NULL;p->next_sibling=NULL;}')
    prefix = prefix.replace(
        'static void vmm_destroy_address_space(void *root)\n'
        '{assert(root==child.address_space);destroyed_roots++;}',
        'static void vmm_destroy_address_space(void *root)\n'
        '{assert(root==child.address_space); assert('
        'fixture_interrupts_enabled && !incoming_mutex_pending && '
        '!in_scheduler_callback); destroyed_roots++;}')
    return prefix


prefix = fixture_prefix()
prefix += r'''
static bool process_cleanup_worker_wake(process_t *process)
{
    if (!process) {
        assert(!process);
        assert(atomic_load_explicit(&process_session_collection_retry_pending,
                                    memory_order_acquire));
        atomic_fetch_add_explicit(&collection_retry_wake_count,1U,
                                  memory_order_relaxed);
        return true;
    }
    if (!process || !publisher_context || !in_scheduler_callback ||
        fixture_interrupts_enabled || !incoming_mutex_pending)
        fprintf(stderr,"wake context process=%p publisher=%d callback=%d IF=%d mutex=%d\n",
                (void *)process,publisher_context,in_scheduler_callback,
                fixture_interrupts_enabled,incoming_mutex_pending);
    assert(process && publisher_context && in_scheduler_callback &&
           !fixture_interrupts_enabled && incoming_mutex_pending);
    if (pause_this_wake) {
        pthread_mutex_lock(&wake_gate_lock);
        wake_entered=true;
        pthread_cond_broadcast(&wake_gate_changed);
        while (!release_paused_wake)
            pthread_cond_wait(&wake_gate_changed,&wake_gate_lock);
        pthread_mutex_unlock(&wake_gate_lock);
    }
    atomic_fetch_add_explicit(&wake_count,1U,memory_order_relaxed);
    return true;
}
'''

process_source = (ROOT / 'kernel/src/process.c').read_text()
object_source = (ROOT / 'kernel/src/object.c').read_text()
scheduler_source = (ROOT / 'kernel/src/scheduler.c').read_text()
completion = c_function(scheduler_source,
                        'scheduler_thread_handoff_callback_valid')
completion = completion.replace('bool scheduler_thread_handoff_callback_valid(',
                                'static bool scheduler_thread_handoff_callback_valid(', 1)
thread_state_snapshot = c_function(
    scheduler_source, 'scheduler_thread_state_snapshot')
functions = completion + thread_state_snapshot + r'''
static bool scheduler_thread_exit_complete(const kernel_thread_t *thread)
{return thread && thread->state==THREAD_STATE_TERMINATED &&
 thread->running_cpu==THREAD_CPU_NONE && !thread->context_switch_pending &&
 !thread->sleeping && !thread->preempt_return_rip;}
'''
functions += c_function(object_source, 'object_reference')
actual_release = c_function(object_source, 'object_release').replace(
    'void object_release(', 'static void process_object_release_actual(', 1)
functions += actual_release + r'''
static bool pause_before_reference_drop;
static void object_release(kernel_object_t *object)
{
    if (pause_before_reference_drop && object==&child.object) {
        pause_before_reference_drop=false;
        assert(process_finish_termination(&child));
        process_reap_session_members(42);
        assert(child.state==PROCESS_STATE_TERMINATED &&
               child.thread_count==1 && child.object.ref_count==3 &&
               child.owner_reference_held && !destroyed_roots);
    }
    process_object_release_actual(object);
}
'''
for name in (
        'process_storage_authorization_clear_locked',
        'process_thread_list_complete_locked',
        'process_thread_member_locked',
        'process_session_collection_ready_locked',
        'process_worker_records_release_all',
        'process_waiter_wake_locked',
        'process_publish_terminated_locked',
        'process_finish_termination',
        'process_scheduler_handoff_complete_internal',
        'process_scheduler_handoff_complete',
        'process_scheduler_context_handoff_complete',
        'process_scheduler_post_handoff',
        'process_detach_thread_owned',
        'process_detach_thread',
        'process_finalize_exit',
        'process_wait',
        'process_collect_completed', 'process_reap_session_members',
        'process_cleanup_context_valid',
        'process_cleanup_worker_request_session_collection_retry',
        'process_cleanup_worker_retry_session_collections_once',
        'process_start_cleanup_worker',
        'process_handle_reference_release',
        'handle_slots',
        'process_handle_revision_advance',
        'decode_handle',
        'process_handle_lookup_reference',
        'process_handle_close',
        'process_handle_pid',
        'process_object_destroy',
        'process_cleanup_reference_candidate_locked', 'process_cleanup_worker_release_handoffs_once',
        'process_cleanup_worker_release_deferred_references_once'):
    functions += c_function(process_source, name)
functions += c_function(scheduler_source,
                         'scheduler_context_switch_post_complete')

main = r'''
static void reset(bool active)
{
    memset(&parent,0,sizeof(parent));
    memset(&child,0,sizeof(child));
    memset(&parent_thread,0,sizeof(parent_thread));
    memset(&child_thread,0,sizeof(child_thread));
    parent.state=PROCESS_STATE_ACTIVE;
    child.state=active?PROCESS_STATE_ACTIVE:PROCESS_STATE_TERMINATED;
    child.parent=&parent;
    child.main_thread=&child_thread;
    child.threads=&child_thread;
    child.thread_count=1;
    child.live_thread_count=active?1U:0U;
    child.exit_cleanup_complete=!active;
    child.session_id=42;
    child.launch_ready=true;
    child.owner_reference_held=true;
    child.exit_status=23;
    child.object=(kernel_object_t){.ref_count=2,.process=&child,
        .type=OBJECT_TYPE_PROCESS};
    child_thread=(kernel_thread_t){
        .state=active?THREAD_STATE_READY:THREAD_STATE_TERMINATED,
        .running_cpu=THREAD_CPU_NONE,
        .process=&child,
        .process_state=active?THREAD_PROCESS_ATTACHED:
                              THREAD_PROCESS_HANDOFF_COMPLETE,
        .process_initial=true,
        .process_reference_held=true};
    all_processes=&child;
    process_registry_lock=scheduler_lock=sleep_calls=owner_releases=step=0;
    session_fixture_lock=0;
    fixture_lock_depth=0;
    block_calls=exit_notifications=thread_reaps=0;
    destroyed_roots=released_memory=frees=external_terminations=0;
    user_region_metadata_releases=0;
    cancel_initial=expected_panic=false;
    block_success=sleep_success=true;
    request_shutdown=false;
    fixture_interrupts_enabled=true;
    in_cleanup_worker=in_scheduler_callback=incoming_mutex_pending=false;
    atomic_store_explicit(&wake_count,0U,memory_order_relaxed);
    atomic_store_explicit(&publishers_done,0U,memory_order_relaxed);
    atomic_store_explicit(&process_session_collection_retry_pending,false,
                          memory_order_relaxed);
    atomic_store_explicit(&process_cleanup_worker_ready,true,
                          memory_order_relaxed);
    atomic_store_explicit(&collection_retry_wake_count,0U,
                          memory_order_relaxed);
    process_destructions=0;
    unsafe_destructor_entries=0;
    pause_before_reference_drop=false;
    pause_pid_lookup=pid_lookup_pinned=pid_close_done=false;
    helper_snapshot_paused=termination_published=false;
    retry_scan_paused=retry_published=false;
    collection_role=0;
    collector_selected=worker_pin_paused=collector_done=false;
}

static void prepare_handoff(process_t *process,kernel_thread_t *thread)
{
    thread->context_switch_pending=true;
    assert(object_reference(&process->object));
    assert(process_scheduler_context_handoff_complete(thread));
    thread->context_switch_pending=false;
}

static void enter_callback_context(void)
{
    publisher_context=true;
    in_scheduler_callback=true;
    incoming_mutex_pending=true;
    fixture_interrupts_enabled=false;
}

static void leave_callback_context(void)
{
    fixture_interrupts_enabled=true;
    incoming_mutex_pending=false;
    in_scheduler_callback=false;
    publisher_context=false;
}

static void enter_worker_context(void)
{
    in_cleanup_worker=true;
    fixture_interrupts_enabled=true;
    incoming_mutex_pending=false;
    in_scheduler_callback=false;
}

static void reduce_to_owner_reference(void)
{
    child.thread_count=0;
    child.live_thread_count=0;
    child.threads=NULL;
    child.main_thread=NULL;
    child_thread.process=NULL;
    child.object.ref_count=1;
}

static void leave_worker_context(void)
{
    in_cleanup_worker=false;
}

static void drain_all_pending(void)
{
    enter_worker_context();
    while (process_cleanup_worker_release_handoffs_once()) { }
    leave_worker_context();
}

static void exact_last_reference_case(void)
{
    i32 status=-1;
    reset(false);
    child.object.destroy=process_object_destroy;
    child.object.process=&child;
    child.address_space=&child;

    /* Register the actual scheduler-retained reference while the outgoing
     * record is still in its protected handoff window. */
    prepare_handoff(&child,&child_thread);
    assert(child.scheduler_handoff_references==1 &&
           child.object.ref_count==3);
    assert(object_reference(&child.object)); /* parent handle */

    /* CPU B runs actual wait/finalization and closes its handle. The hook
     * token on CPU A is now the process's only remaining reference. */
    assert(process_wait(&parent,7,&status)==MG_OK && status==23);
    assert(child.thread_count==0 && !child.owner_reference_held &&
           child.object.ref_count==2 && child.scheduler_handoff_references==1 &&
           !atomic_load_explicit(&child.deferred_reference_releases_pending,
                                 memory_order_acquire));
    object_release(&child.object);
    assert(child.object.ref_count==1 && destroyed_roots==0);

    enter_callback_context();
    scheduler_context_switch_post_complete(&child);
    assert(child.object.ref_count==1 && destroyed_roots==0 &&
           child.scheduler_handoff_references==1 &&
           atomic_load_explicit(&child.scheduler_handoff_releases_pending,
                                memory_order_acquire)==1);
    leave_callback_context();

    enter_worker_context();
    assert(process_cleanup_worker_release_handoffs_once());
    assert(child.object.ref_count==0 && destroyed_roots==1 &&
           child.scheduler_handoff_references==0 &&
           !atomic_load_explicit(&child.scheduler_handoff_releases_pending,
                                 memory_order_acquire) &&
           !atomic_load_explicit(&child.deferred_reference_releases_pending,
                                 memory_order_acquire));
    leave_worker_context();
    assert(released_memory==1 &&
           user_region_metadata_releases==1);
}

static void session_collection_case(void)
{
    reset(false);
    child.object.destroy=process_object_destroy;
    child.object.process=&child;
    child.address_space=&child;
    child.session_id=42;
    prepare_handoff(&child,&child_thread);
    process_reap_session_members(42);
    assert(!child.owner_reference_held && child.thread_count==0 &&
           child.object.ref_count==1 &&
           child.scheduler_handoff_references==1 && destroyed_roots==0);
    enter_callback_context();
    scheduler_context_switch_post_complete(&child);
    leave_callback_context();
    enter_worker_context();
    assert(process_cleanup_worker_release_handoffs_once());
    leave_worker_context();
    assert(child.object.ref_count==0 && destroyed_roots==1 &&
           child.scheduler_handoff_references==0);
}

typedef struct { pthread_barrier_t *start; bool pause_wake; } publisher_t;
static void *publish_handoff(void *argument)
{
    publisher_t *publisher=argument;
    pause_this_wake=publisher->pause_wake;
    enter_callback_context();
    int result=pthread_barrier_wait(publisher->start);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    scheduler_context_switch_post_complete(&child);
    leave_callback_context();
    pause_this_wake=false;
    atomic_fetch_add_explicit(&publishers_done,1U,memory_order_release);
    return NULL;
}

static void publication_before_wake_case(void)
{
    pthread_t publisher_thread;
    pthread_barrier_t start;
    publisher_t publisher={.start=&start,.pause_wake=true};
    reset(false);
    reduce_to_owner_reference();
    atomic_store_explicit(&wake_count,0U,memory_order_relaxed);
    atomic_store_explicit(&publishers_done,0U,memory_order_relaxed);
    child.object.destroy=process_object_destroy;
    child.object.process=&child;
    child.address_space=&child;
    assert(object_reference(&child.object));
    child.scheduler_handoff_references=1;
    assert(pthread_barrier_init(&start,NULL,2)==0);
    assert(pthread_create(&publisher_thread,NULL,publish_handoff,&publisher)==0);
    int result=pthread_barrier_wait(&start);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    pthread_barrier_destroy(&start);

    pthread_mutex_lock(&wake_gate_lock);
    while (!wake_entered)
        pthread_cond_wait(&wake_gate_changed,&wake_gate_lock);
    pthread_mutex_unlock(&wake_gate_lock);
    assert(atomic_load_explicit(&child.scheduler_handoff_releases_pending,
                                memory_order_acquire)==1 &&
           atomic_load_explicit(&wake_count,memory_order_relaxed)==0);
    drain_all_pending();
    assert(child.object.ref_count==1 && child.scheduler_handoff_references==0 &&
           destroyed_roots==0);
    pthread_mutex_lock(&wake_gate_lock);
    release_paused_wake=true;
    pthread_cond_broadcast(&wake_gate_changed);
    pthread_mutex_unlock(&wake_gate_lock);
    assert(pthread_join(publisher_thread,NULL)==0);
    assert(atomic_load_explicit(&wake_count,memory_order_relaxed)==1 &&
           atomic_load_explicit(&child.scheduler_handoff_releases_pending,
                                memory_order_acquire)==0);
    enter_worker_context();object_release(&child.object);leave_worker_context();
    assert(destroyed_roots==1);
}

#define PUBLISHER_COUNT 41U
static void concurrent_publishers_case(void)
{
    pthread_t threads[PUBLISHER_COUNT];
    publisher_t publishers[PUBLISHER_COUNT];
    pthread_barrier_t start;
    reset(false);
    reduce_to_owner_reference();
    atomic_store_explicit(&wake_count,0U,memory_order_relaxed);
    atomic_store_explicit(&publishers_done,0U,memory_order_relaxed);
    child.object.destroy=process_object_destroy;
    child.object.process=&child;
    child.address_space=&child;
    all_processes=&child;
    for (u32 i=0;i<PUBLISHER_COUNT;i++)
        assert(object_reference(&child.object));
    child.scheduler_handoff_references=PUBLISHER_COUNT;
    assert(pthread_barrier_init(&start,NULL,PUBLISHER_COUNT+1U)==0);
    for (u32 i=0;i<PUBLISHER_COUNT;i++) {
        publishers[i]=(publisher_t){.start=&start};
        assert(pthread_create(&threads[i],NULL,publish_handoff,
                              &publishers[i])==0);
    }
    int result=pthread_barrier_wait(&start);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    pthread_barrier_destroy(&start);
    enter_worker_context();
    for (;;) {
        bool done=atomic_load_explicit(&publishers_done,
                                       memory_order_acquire)==
                  PUBLISHER_COUNT;
        if (done &&
            atomic_load_explicit(&child.scheduler_handoff_releases_pending,
                                 memory_order_acquire)==0)
            break;
        (void)process_cleanup_worker_release_handoffs_once();
        sched_yield();
    }
    for (u32 i=0;i<PUBLISHER_COUNT;i++)
        assert(pthread_join(threads[i],NULL)==0);
    while (process_cleanup_worker_release_handoffs_once()) { }
    leave_worker_context();
    assert(child.object.ref_count==1 && child.scheduler_handoff_references==0 &&
           atomic_load_explicit(&child.scheduler_handoff_releases_pending,
                                memory_order_acquire)==0 &&
           atomic_load_explicit(&wake_count,memory_order_relaxed)==
               PUBLISHER_COUNT);
    enter_worker_context();object_release(&child.object);leave_worker_context();
    assert(destroyed_roots==1);
}

static void *collect_session_paused(void *unused)
{
    (void)unused;
    collection_role=1;
    fixture_interrupts_enabled=true;
    process_reap_session_members(42);
    assert(child.thread_count==0 && child.owner_reference_held &&
           child.object.ref_count==2 && destroyed_roots==0);
    assert(!pthread_mutex_lock(&collection_gate));
    collector_done=true;
    pthread_cond_broadcast(&collection_changed);
    assert(!pthread_mutex_unlock(&collection_gate));
    return NULL;
}

static void *release_handoff_during_collection(void *unused)
{
    (void)unused;
    collection_role=2;
    enter_worker_context();
    assert(process_cleanup_worker_release_handoffs_once());
    leave_worker_context();
    return NULL;
}

static void worker_pin_overlapping_session_collection_case(void)
{
    pthread_t collector, worker;
    reset(false);
    child.object.destroy=process_object_destroy;
    child.object.process=&child;
    child.address_space=&child;
    prepare_handoff(&child,&child_thread);
    enter_callback_context();
    scheduler_context_switch_post_complete(&child);
    leave_callback_context();

    assert(!pthread_create(&collector,NULL,collect_session_paused,NULL));
    assert(!pthread_mutex_lock(&collection_gate));
    while(!collector_selected)
        pthread_cond_wait(&collection_changed,&collection_gate);
    assert(!pthread_mutex_unlock(&collection_gate));
    assert(!pthread_create(&worker,NULL,release_handoff_during_collection,NULL));
    assert(!pthread_join(collector,NULL));
    assert(!pthread_join(worker,NULL));
    collection_role=0;

    assert(child.thread_count==0 && !child.owner_reference_held &&
           child.object.ref_count==0 && !child.scheduler_handoff_references &&
           !atomic_load_explicit(&child.scheduler_handoff_releases_pending,
                                 memory_order_acquire) &&
           destroyed_roots==1 && released_memory==1 &&
           user_region_metadata_releases==1 && all_processes==NULL);
}

static pthread_barrier_t competing_collectors_start;
static void *collect_finalized_session(void *unused)
{
    (void)unused;
    enter_worker_context();
    int result=pthread_barrier_wait(&competing_collectors_start);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    process_reap_session_members(42);
    leave_worker_context();
    return NULL;
}

static void concurrent_session_collectors_case(void)
{
    pthread_t collectors[2];
    reset(false);
    child.object.destroy=process_object_destroy;
    child.object.process=&child;
    child.address_space=&child;
    assert(process_finalize_exit(&child));
    assert(child.thread_count==0 && child.object.ref_count==1);
    assert(pthread_barrier_init(&competing_collectors_start,NULL,3)==0);
    for(unsigned i=0;i<2;i++)
        assert(!pthread_create(&collectors[i],NULL,
                              collect_finalized_session,NULL));
    int result=pthread_barrier_wait(&competing_collectors_start);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(!pthread_join(collectors[0],NULL));
    assert(!pthread_join(collectors[1],NULL));
    pthread_barrier_destroy(&competing_collectors_start);
    assert(!child.owner_reference_held && child.object.ref_count==0 &&
           owner_releases==0 && destroyed_roots==1 &&
           user_region_metadata_releases==1 && all_processes==NULL);
}

static void delayed_handle_close_and_operation_case(void)
{
    reset(false);
    enter_worker_context(); /* Session collection is a safe teardown context. */
    child.object.destroy=process_object_destroy;
    child.object.process=&child;
    child.address_space=&child;
    /* Owner, final thread, table slot, close lookup, and an operation lookup. */
    child.object.ref_count=5;
    process_reap_session_members(42);
    assert(child.thread_count==1 && child.owner_reference_held &&
           !destroyed_roots);

    object_release(&child.object); /* close removes the table slot */
    process_handle_reference_release(&child.object); /* close lookup */
    assert(child.thread_count==1 && child.owner_reference_held &&
           child.object.ref_count==3 && !destroyed_roots);

    /* An operation that retained the handle before close is the last external
     * pin. Dropping it retries collection only after the pin is gone. */
    process_handle_reference_release(&child.object);
    assert(child.thread_count==0 && !child.owner_reference_held &&
           child.object.ref_count==0 && destroyed_roots==1 &&
           user_region_metadata_releases==1 && all_processes==NULL);
    leave_worker_context();
}

static void *release_stale_snapshot_reference(void *unused)
{
    (void)unused;
    collection_role=3;
    fixture_interrupts_enabled=true;
    process_handle_reference_release(&child.object);
    return NULL;
}

static void *publish_termination_during_release(void *unused)
{
    (void)unused;
    collection_role=4;
    enter_worker_context();
    pthread_mutex_lock(&collection_gate);
    while(!helper_snapshot_paused)
        pthread_cond_wait(&collection_changed,&collection_gate);
    pthread_mutex_unlock(&collection_gate);

    /* This is the real termination publisher and collector. The helper's
     * reference is still live, so exact-count admission must defer. */
    assert(process_finish_termination(&child));
    process_reap_session_members(42);
    assert(child.state==PROCESS_STATE_TERMINATED && child.thread_count==1 &&
           child.object.ref_count==3 && child.owner_reference_held &&
           !destroyed_roots);
    pthread_mutex_lock(&collection_gate);
    termination_published=true;
    pthread_cond_broadcast(&collection_changed);
    pthread_mutex_unlock(&collection_gate);
    leave_worker_context();
    return NULL;
}

static void termination_between_snapshot_and_drop_case(void)
{
    pthread_t releaser,terminator;
    reset(false);
    child.state=PROCESS_STATE_EXITING;
    child.exit_notification_sent=false;
    child.object.ref_count=3; /* owner, attached thread, retained lookup */
    child.object.destroy=process_object_destroy;
    child.address_space=&child;
    child.parent=&parent;parent.first_child=&child;
    child.main_thread=&child_thread;child.threads=&child_thread;
    child.thread_count=1;child.live_thread_count=0;
    child_thread.process=&child;
    child_thread.process_state=THREAD_PROCESS_HANDOFF_COMPLETE;
    child_thread.process_reference_held=true;
    child_thread.stack_external=true;
    assert(!pthread_create(&releaser,NULL,
                           release_stale_snapshot_reference,NULL));
    assert(!pthread_create(&terminator,NULL,
                           publish_termination_during_release,NULL));
    assert(!pthread_join(releaser,NULL));
    assert(!pthread_join(terminator,NULL));
    assert(child.state==PROCESS_STATE_TERMINATED && !child.thread_count &&
           !child.owner_reference_held && child.object.ref_count==0 &&
           all_processes==NULL && !parent.first_child &&
           destroyed_roots==1 && released_memory==1 &&
           user_region_metadata_releases==1 && process_destructions==1);
}

static void *pid_lookup_under_session_lock(void *unused)
{
    (void)unused;
    collection_role=6;
    fixture_interrupts_enabled=true;
    u64 flags=spin_lock_irqsave(&session_fixture_lock);
    assert(process_handle_pid(&parent,0x10001U)==123U);
    spin_unlock_irqrestore(&session_fixture_lock,flags);
    return NULL;
}

static void prepare_finalized_session_process(void)
{
    reset(false);
    child.object.destroy=process_object_destroy;
    child.address_space=&child;
    child.parent=&parent;parent.first_child=&child;
    child.pid=123U;
    assert(process_finalize_exit(&child));
    assert(child.state==PROCESS_STATE_TERMINATED && !child.thread_count &&
           child.object.ref_count==1 && child.exit_notification_sent);
}

static void locked_pid_release_deferred_case(void)
{
    pthread_t lookup;
    prepare_finalized_session_process();
    assert(!pthread_mutex_init(&parent.handle_growth_mutex,NULL));
    pid_handle_slot=(process_handle_slot_t){.object=&child.object,
        .generation=1U,.active=true};
    parent.handle_table=&pid_handle_slot;
    parent.handle_capacity=1U;
    parent.handle_revision=1U;
    assert(object_reference(&child.object)); /* open handle-table slot */
    process_reap_session_members(42);
    assert(child.owner_reference_held && child.object.ref_count==2 &&
           !destroyed_roots);

    pause_pid_lookup=true;
    assert(!pthread_create(&lookup,NULL,pid_lookup_under_session_lock,NULL));
    pthread_mutex_lock(&pid_gate_lock);
    while(!pid_lookup_pinned)
        pthread_cond_wait(&pid_gate_changed,&pid_gate_lock);
    pthread_mutex_unlock(&pid_gate_lock);

    /* CPU B uses the production close path while CPU A still has the PID
     * lookup pin and holds session_lock. The close lookup's safe retry must
     * decline because the PID pin still owns the process. */
    fixture_interrupts_enabled=true;
    assert(process_handle_close(&parent,0x10001U));
    assert(!pid_handle_slot.active && !pid_handle_slot.object);
    assert(child.owner_reference_held && child.object.ref_count==2 &&
           !destroyed_roots);
    pthread_mutex_lock(&pid_gate_lock);
    pid_close_done=true;
    pthread_cond_broadcast(&pid_gate_changed);
    pthread_mutex_unlock(&pid_gate_lock);
    assert(!pthread_join(lookup,NULL));

    assert(child.owner_reference_held && child.object.ref_count==2 &&
           !destroyed_roots &&
           atomic_load_explicit(&child.deferred_reference_releases_pending,
                                memory_order_acquire)==1U &&
           atomic_load_explicit(&process_session_collection_retry_pending,
                                memory_order_acquire) &&
           atomic_load_explicit(&collection_retry_wake_count,
                                memory_order_relaxed)==1U);

    /* Duplicate notifications coalesce without another wake or retained pin. */
    for(unsigned i=0;i<8;i++)
        process_cleanup_worker_request_session_collection_retry();
    assert(atomic_load_explicit(&collection_retry_wake_count,
                                memory_order_relaxed)==1U);
    enter_worker_context();
    assert(process_cleanup_worker_release_deferred_references_once());
    assert(process_cleanup_worker_retry_session_collections_once());
    assert(!process_cleanup_worker_retry_session_collections_once());
    assert(!child.owner_reference_held && child.object.ref_count==0 &&
           all_processes==NULL && !parent.first_child &&
           destroyed_roots==1 && released_memory==1 &&
           user_region_metadata_releases==1 && process_destructions==1 &&
           !atomic_load_explicit(&process_session_collection_retry_pending,
                                 memory_order_acquire));
    leave_worker_context();
    assert(!pthread_mutex_destroy(&parent.handle_growth_mutex));
}

static void wait_and_close_before_locked_pid_release_case(void)
{
    pthread_t lookup;
    i32 status=-1;
    prepare_finalized_session_process();
    assert(!pthread_mutex_init(&parent.handle_growth_mutex,NULL));
    pid_handle_slot=(process_handle_slot_t){.object=&child.object,
        .generation=1U,.active=true};
    parent.handle_table=&pid_handle_slot;
    parent.handle_capacity=1U;
    parent.handle_revision=1U;
    assert(object_reference(&child.object)); /* open handle-table slot */

    pause_pid_lookup=true;
    assert(!pthread_create(&lookup,NULL,pid_lookup_under_session_lock,NULL));
    pthread_mutex_lock(&pid_gate_lock);
    while(!pid_lookup_pinned)
        pthread_cond_wait(&pid_gate_changed,&pid_gate_lock);
    pthread_mutex_unlock(&pid_gate_lock);

    /* The PID query has retained its lookup while holding session_lock. A
     * successful wait consumes the process owner; closing the table handle
     * leaves only the still-paused PID lookup reference. */
    fixture_interrupts_enabled=true;
    assert(process_wait(&parent,0x10001U,&status)==MG_OK && status==23);
    assert(!child.owner_reference_held && child.wait_collected &&
           child.object.ref_count==2 && !destroyed_roots &&
           !atomic_load_explicit(&child.deferred_reference_releases_pending,
                                 memory_order_acquire));
    /* Model the wait and handle-close operations on CPU B, independently of
     * CPU A's interrupt-disabled PID lookup. */
    fixture_interrupts_enabled=true;
    assert(process_handle_close(&parent,0x10001U));
    assert(!pid_handle_slot.active && !pid_handle_slot.object &&
           child.object.ref_count==1 && !destroyed_roots);

    pthread_mutex_lock(&pid_gate_lock);
    pid_close_done=true;
    pthread_cond_broadcast(&pid_gate_changed);
    pthread_mutex_unlock(&pid_gate_lock);
    assert(!pthread_join(lookup,NULL));

    /* process_wait has restored IF before dropping its operation pin, so only
     * the still-locked PID lookup needs a deferred release. Its reference
     * remains owned until the safe worker drains it. */
    assert(!child.owner_reference_held && child.object.ref_count==1 &&
           atomic_load_explicit(&child.deferred_reference_releases_pending,
                                memory_order_acquire)==1U &&
           !destroyed_roots && !unsafe_destructor_entries &&
           atomic_load_explicit(&process_session_collection_retry_pending,
                                memory_order_acquire));
    assert(atomic_load_explicit(&collection_retry_wake_count,
                                memory_order_relaxed)==1U);

    enter_worker_context();
    assert(process_cleanup_worker_release_deferred_references_once());
    assert(!child.object.ref_count &&
           !atomic_load_explicit(&child.deferred_reference_releases_pending,
                                 memory_order_acquire) &&
           !child.deferred_reference_collection_retry &&
           destroyed_roots==1 && process_destructions==1 &&
           !unsafe_destructor_entries && all_processes==NULL);
    assert(!process_cleanup_worker_release_deferred_references_once());
    leave_worker_context();
    assert(!pthread_mutex_destroy(&parent.handle_growth_mutex));
}

typedef struct { pthread_barrier_t *start; } collection_releaser_t;
static void *release_unsafe_collection_pin(void *argument)
{
    collection_releaser_t *item=argument;
    int result=pthread_barrier_wait(item->start);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    fixture_interrupts_enabled=false;
    process_handle_reference_release(&child.object);
    fixture_interrupts_enabled=true;
    return NULL;
}

static void concurrent_pin_releases_coalesce_case(void)
{
    enum {RELEASES=12};
    pthread_t threads[RELEASES];
    collection_releaser_t item;
    pthread_barrier_t start;
    prepare_finalized_session_process();
    for(unsigned i=0;i<RELEASES;i++)
        assert(object_reference(&child.object));
    assert(child.object.ref_count==RELEASES+1U);
    assert(!pthread_barrier_init(&start,NULL,RELEASES+1U));
    item.start=&start;
    for(unsigned i=0;i<RELEASES;i++)
        assert(!pthread_create(&threads[i],NULL,
                               release_unsafe_collection_pin,&item));
    int result=pthread_barrier_wait(&start);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    for(unsigned i=0;i<RELEASES;i++)
        assert(!pthread_join(threads[i],NULL));
    pthread_barrier_destroy(&start);
    assert(child.owner_reference_held &&
           child.object.ref_count==RELEASES+1U &&
           atomic_load_explicit(&child.deferred_reference_releases_pending,
                                memory_order_acquire)==RELEASES &&
           !destroyed_roots &&
           atomic_load_explicit(&process_session_collection_retry_pending,
                                memory_order_acquire) &&
           atomic_load_explicit(&collection_retry_wake_count,
                                memory_order_relaxed)==1U);
    enter_worker_context();
    assert(process_cleanup_worker_release_deferred_references_once());
    assert(process_cleanup_worker_retry_session_collections_once());
    assert(!child.owner_reference_held && !child.object.ref_count &&
           !all_processes && !parent.first_child && destroyed_roots==1 &&
           process_destructions==1);
    leave_worker_context();
}

static void concurrent_deferred_reference_publishers_case(void)
{
    enum {RELEASES=41U};
    pthread_t threads[RELEASES];
    collection_releaser_t item;
    pthread_barrier_t start;
    prepare_finalized_session_process();
    child.session_id=PROCESS_NO_SESSION;
    for(unsigned i=0;i<RELEASES;i++)
        assert(object_reference(&child.object));
    assert(child.object.ref_count==RELEASES+1U);
    assert(!pthread_barrier_init(&start,NULL,RELEASES+1U));
    item.start=&start;
    for(unsigned i=0;i<RELEASES;i++)
        assert(!pthread_create(&threads[i],NULL,
                               release_unsafe_collection_pin,&item));
    int result=pthread_barrier_wait(&start);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    for(unsigned i=0;i<RELEASES;i++)
        assert(!pthread_join(threads[i],NULL));
    pthread_barrier_destroy(&start);
    assert(child.object.ref_count==RELEASES+1U &&
           atomic_load_explicit(&child.deferred_reference_releases_pending,
                                memory_order_acquire)==RELEASES &&
           !destroyed_roots && !unsafe_destructor_entries &&
           atomic_load_explicit(&collection_retry_wake_count,
                                memory_order_relaxed)==1U);

    enter_worker_context();
    unsigned passes=0;
    while(process_cleanup_worker_release_deferred_references_once())
        passes++;
    assert(passes==3U && child.object.ref_count==1U &&
           child.owner_reference_held &&
           !atomic_load_explicit(&child.deferred_reference_releases_pending,
                                 memory_order_acquire) &&
           !child.deferred_reference_collection_retry && !destroyed_roots);
    object_release(&child.object); /* final owner release in safe context */
    assert(!child.object.ref_count && destroyed_roots==1U &&
           process_destructions==1U && !unsafe_destructor_entries);
    leave_worker_context();
}

static void deferred_reference_before_worker_ready_case(void)
{
    prepare_finalized_session_process();
    assert(object_reference(&child.object));
    __atomic_store_n(&process_cleanup_worker,NULL,__ATOMIC_RELEASE);
    __atomic_store_n(&process_cleanup_worker_ready,false,__ATOMIC_RELEASE);
    fixture_interrupts_enabled=false;
    process_handle_reference_release(&child.object);
    fixture_interrupts_enabled=true;
    assert(child.object.ref_count==2U &&
           atomic_load_explicit(&child.deferred_reference_releases_pending,
                                memory_order_acquire)==1U &&
           !destroyed_roots &&
           atomic_load_explicit(&process_session_collection_retry_pending,
                                memory_order_acquire) &&
           atomic_load_explicit(&collection_retry_wake_count,
                                memory_order_relaxed)==0U);

    assert(process_start_cleanup_worker());
    assert(atomic_load_explicit(&process_cleanup_worker_ready,
                                memory_order_acquire) &&
           atomic_load_explicit(&collection_retry_wake_count,
                                memory_order_relaxed)==1U);
    enter_worker_context();
    assert(process_cleanup_worker_release_deferred_references_once());
    assert(!child.owner_reference_held && !child.object.ref_count &&
           !child.deferred_reference_collection_retry &&
           !child.deferred_reference_releases_pending &&
           destroyed_roots==1U && process_destructions==1U &&
           !unsafe_destructor_entries);
    leave_worker_context();
}

static void *retry_scan_during_publication(void *unused)
{
    (void)unused;
    collection_role=7;
    enter_worker_context();
    assert(process_cleanup_worker_retry_session_collections_once());
    /* A ref transferred during the scan is durable work for the next pass,
     * even though the current collection scan already consumed its latch. */
    assert(process_cleanup_worker_release_deferred_references_once());
    leave_worker_context();
    return NULL;
}

static void retry_publication_during_scan_case(void)
{
    pthread_t scanner;
    prepare_finalized_session_process();
    assert(__atomic_exchange_n(&process_session_collection_retry_pending,true,
                               __ATOMIC_RELEASE)==false);
    assert(!pthread_create(&scanner,NULL,retry_scan_during_publication,NULL));
    pthread_mutex_lock(&collection_gate);
    while(!retry_scan_paused)
        pthread_cond_wait(&collection_changed,&collection_gate);
    pthread_mutex_unlock(&collection_gate);

    assert(object_reference(&child.object));
    fixture_interrupts_enabled=false;
    process_handle_reference_release(&child.object);
    fixture_interrupts_enabled=true;
    pthread_mutex_lock(&collection_gate);
    retry_published=true;
    pthread_cond_broadcast(&collection_changed);
    pthread_mutex_unlock(&collection_gate);
    assert(!pthread_join(scanner,NULL));
    assert(!child.owner_reference_held && child.object.ref_count==0 &&
           process_destructions==1 && !unsafe_destructor_entries &&
           atomic_load_explicit(&process_session_collection_retry_pending,
                                memory_order_acquire));
    enter_worker_context();
    assert(process_cleanup_worker_retry_session_collections_once());
    assert(!atomic_load_explicit(&process_session_collection_retry_pending,
                                 memory_order_acquire) &&
           process_destructions==1);
    leave_worker_context();
}

static void retry_latched_before_worker_start_case(void)
{
    prepare_finalized_session_process();
    __atomic_store_n(&process_cleanup_worker,NULL,__ATOMIC_RELEASE);
    __atomic_store_n(&process_cleanup_worker_ready,false,__ATOMIC_RELEASE);
    process_cleanup_worker_request_session_collection_retry();
    assert(atomic_load_explicit(&process_session_collection_retry_pending,
                                memory_order_acquire) &&
           atomic_load_explicit(&collection_retry_wake_count,
                                memory_order_relaxed)==0U &&
           child.owner_reference_held);

    /* The production startup path publishes the worker as ready, then checks
     * the durable latch and wakes it if a request arrived before startup. */
    assert(process_start_cleanup_worker());
    assert(atomic_load_explicit(&process_cleanup_worker_ready,
                                memory_order_acquire) &&
           atomic_load_explicit(&collection_retry_wake_count,
                                memory_order_relaxed)==1U &&
           atomic_load_explicit(&process_session_collection_retry_pending,
                                memory_order_acquire));
    enter_worker_context();
    assert(process_cleanup_worker_retry_session_collections_once());
    assert(!child.owner_reference_held && !child.object.ref_count &&
           !all_processes && process_destructions==1);
    leave_worker_context();
}

static void fair_reference_batches_case(void)
{
    reset(true);
    enter_worker_context();
    process_t records[20];memset(records,0,sizeof(records));
    for(unsigned i=0;i<20;i++) {
        records[i].pid=i+1;
        records[i].next_all=i?&records[i-1]:NULL;
        records[i].object=(kernel_object_t){.ref_count=2,.process=&records[i],
            .type=OBJECT_TYPE_PROCESS};
        records[i].state=PROCESS_STATE_ACTIVE;
        records[i].deferred_reference_releases_pending=1;
    }
    all_processes=&records[19];
    records[19].object.ref_count=65;
    records[19].deferred_reference_releases_pending=64;
    process_reference_release_after=process_reference_release_limit=0;
    for(unsigned i=0;i<20;i++)
        assert(process_cleanup_worker_release_deferred_references_once());
    for(unsigned i=0;i<19;i++)
        assert(!records[i].deferred_reference_releases_pending &&
               records[i].object.ref_count==1);
    assert(records[19].deferred_reference_releases_pending==48);
    for(unsigned i=0;i<20;i++) {
        records[i].scheduler_handoff_references=1;
        records[i].scheduler_handoff_releases_pending=1;
        records[i].object.ref_count++;
    }
    records[19].object.ref_count+=63;
    records[19].scheduler_handoff_references=64;
    records[19].scheduler_handoff_releases_pending=64;
    process_handoff_release_after=process_handoff_release_limit=0;
    for(unsigned i=0;i<20;i++)
        assert(process_cleanup_worker_release_handoffs_once());
    for(unsigned i=0;i<19;i++)
        assert(!records[i].scheduler_handoff_releases_pending &&
               records[i].object.ref_count==1);
    assert(records[19].scheduler_handoff_releases_pending==48);
    process_t late={0};late.pid=21;late.next_all=all_processes;
    late.object=(kernel_object_t){.ref_count=2,.process=&late,
        .type=OBJECT_TYPE_PROCESS};
    late.scheduler_handoff_references=late.scheduler_handoff_releases_pending=1;
    all_processes=&late;
    records[0].object.ref_count++;
    records[0].scheduler_handoff_references=records[0].scheduler_handoff_releases_pending=1;
    process_handoff_release_after=process_handoff_release_limit=20;
    assert(process_cleanup_worker_release_handoffs_once());
    assert(!records[0].scheduler_handoff_releases_pending &&
           late.scheduler_handoff_releases_pending==1);
    all_processes=NULL;
    leave_worker_context();
}

int main(void)
{
    fair_reference_batches_case();
    exact_last_reference_case();
    session_collection_case();
    worker_pin_overlapping_session_collection_case();
    concurrent_session_collectors_case();
    delayed_handle_close_and_operation_case();
    publication_before_wake_case();
    concurrent_publishers_case();
    termination_between_snapshot_and_drop_case();
    locked_pid_release_deferred_case();
    wait_and_close_before_locked_pid_release_case();
    concurrent_pin_releases_coalesce_case();
    concurrent_deferred_reference_publishers_case();
    deferred_reference_before_worker_ready_case();
    retry_publication_during_scan_case();
    retry_latched_before_worker_start_case();
    puts("deferred scheduler references: final release context, session reap, "
         "zero-thread retry, stale-state and locked-caller retry, "
         "coalesced concurrent releases, retained unsafe releases, "
         "bounded concurrent reference batches, pre-ready publication, "
         "scan-time publication, "
         "publish-before-wake, bounded batches, and concurrent publishers PASS");
    return 0;
}
'''

code = prefix + functions + main
with HostFixture("scheduler-deferred-release") as workspace:
    directory = workspace.path
    directory = Path(directory)
    source = directory / 'deferred_release.c'
    binary = directory / 'deferred_release'
    source.write_text('#define PROCESS_FIXTURE_REAL_COLLECTION_LATCH\n#include "' + str(ROOT / "tests/kernel/process_cleanup_fixture.h") + '"\n' + code)
    env = dict(os.environ)
    env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', 'detect_leaks=0')
    workspace.run([
        CC, '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-unused-variable',
        '-fsanitize=address,undefined', '-pthread', str(source), '-o',
        str(binary)], check=True, env=env)
    workspace.run([str(binary)], check=True, env=env, timeout=30)
