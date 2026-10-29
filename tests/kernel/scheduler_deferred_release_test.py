#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production scheduler-reference transfer and cleanup-worker drain."""
from pathlib import Path
import ast
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
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

    prefix = prefix.replace('#include <setjmp.h>',
                            '#include <setjmp.h>\n#include <pthread.h>\n'
                            '#include <stdatomic.h>\n#include <sched.h>')
    prefix = prefix.replace('#define PROCESS_NO_SESSION 0',
                            '#define PROCESS_NO_SESSION 0\n'
                            '#define PROCESS_HANDOFF_RELEASE_BATCH 16U')
    prefix = prefix.replace(
        'static process_t parent, child, *all_processes;',
        'static process_t parent, child, *all_processes;\n'
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
        'static void process_object_destroy(kernel_object_t *);\n'
        'static void process_reap_session_members(mg_session_id_t);')
    prefix = prefix.replace(
        'typedef struct {unsigned ref_count; process_t *process;} kernel_object_t;',
        'typedef struct kernel_object {unsigned ref_count; process_t *process; '
        'void (*destroy)(struct kernel_object *); } kernel_object_t;')
    # Use the real atomic object reference implementation below.
    prefix = prefix.replace(c_function(prefix, 'object_reference'), '')
    prefix = prefix.replace(c_function(prefix, 'object_release'), '')
    prefix = prefix.replace(
        'static void process_cwd_clear(process_t *p){assert(p==&child);}',
        'static void process_cwd_clear(process_t *p){assert(p==&child); '
        'assert(in_cleanup_worker && fixture_interrupts_enabled && '
        '!incoming_mutex_pending && !in_scheduler_callback);}')
    prefix = prefix.replace(
        'static void vmm_destroy_address_space(void *root)\n'
        '{assert(root==child.address_space);destroyed_roots++;}',
        'static void vmm_destroy_address_space(void *root)\n'
        '{assert(root==child.address_space); assert(in_cleanup_worker && '
        'fixture_interrupts_enabled && !incoming_mutex_pending && '
        '!in_scheduler_callback); destroyed_roots++;}')
    return prefix


prefix = fixture_prefix()
prefix += r'''
static bool process_cleanup_worker_wake(process_t *process)
{
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
functions = completion + r'''
static bool scheduler_thread_exit_complete(const kernel_thread_t *thread)
{return thread && thread->state==THREAD_STATE_TERMINATED &&
 thread->running_cpu==THREAD_CPU_NONE && !thread->context_switch_pending &&
 !thread->sleeping && !thread->preempt_return_rip;}
'''
functions += c_function(object_source, 'object_reference')
functions += c_function(object_source, 'object_release')
for name in (
        'process_storage_authorization_clear_locked',
        'process_thread_list_complete_locked',
        'process_worker_records_release_all',
        'process_publish_terminated_locked',
        'process_scheduler_handoff_complete_internal',
        'process_scheduler_handoff_complete',
        'process_scheduler_context_handoff_complete',
        'process_scheduler_post_handoff',
        'process_detach_thread_owned',
        'process_detach_thread',
        'process_finalize_exit',
        'process_wait',
        'process_reap_session_members',
        'process_object_destroy',
        'process_cleanup_worker_release_handoffs_once'):
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
    child.object=(kernel_object_t){.ref_count=2,.process=&child};
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
           child.object.ref_count==2 && child.scheduler_handoff_references==1);
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
    leave_worker_context();
    assert(child.object.ref_count==0 && child.scheduler_handoff_references==0 &&
           atomic_load_explicit(&child.scheduler_handoff_releases_pending,
                                memory_order_acquire)==0 &&
           destroyed_roots==1 && released_memory==1 &&
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

int main(void)
{
    exact_last_reference_case();
    session_collection_case();
    publication_before_wake_case();
    concurrent_publishers_case();
    puts("deferred scheduler references: final release context, session reap, "
         "publish-before-wake, bounded batches, and concurrent publishers PASS");
    return 0;
}
'''

code = prefix + functions + main
with tempfile.TemporaryDirectory(prefix='mangrove-deferred-ref-') as directory:
    directory = Path(directory)
    source = directory / 'deferred_release.c'
    binary = directory / 'deferred_release'
    source.write_text(code)
    env = dict(os.environ)
    env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', 'detect_leaks=0')
    subprocess.run([
        CC, '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-unused-variable',
        '-fsanitize=address,undefined', '-pthread', str(source), '-o',
        str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env, timeout=30)
