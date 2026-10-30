#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production worker dispatch/reaper progresses without target syscall activity."""
import ast
from pathlib import Path
import os
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/shared'))
from host_fixture import HostFixture
CC = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
# Reuse only the existing fixture's definitions and production extraction;
# its top-level test execution is excluded, as in the handoff fixtures.
p = ROOT / 'tests/kernel/process_thread_api_test.py'
tree = ast.parse(p.read_text())
tree.body = [node for node in tree.body if not isinstance(node, ast.With)]
namespace = {'__file__': str(p), '__name__': 'fixture_definitions'}
exec(compile(tree, str(p), 'exec'), namespace)
code = namespace['code'].split('int main(')[0]
code = code.replace('struct process {', 'struct process {\n u64 pid, session_id; bool external_termination_in_progress; process_t *next_all;')
code = 'static unsigned record_destructions;\n#include "' + str(ROOT / 'kernel/include/process_cleanup.h') + '"\n' + code
code = code.replace('for(process_user_region_t*r=test_process->user_regions;r;', 'for(process_user_region_t*r=((process_t*)as)->user_regions;r;')
code = code.replace('    kfree(record);', '    record_destructions++; kfree(record);')
code += r'''
static process_t *all_processes;
static bool process_cleanup_worker_ready;
static unsigned process_cleanup_work_pending;
static u64 process_worker_reap_generation;
static u64 process_worker_reap_after, process_worker_reap_limit;
static u64 process_worker_reap_seen_generation;
static bool process_worker_reap_sweep_active;
static unsigned worker_wakes;
static bool process_cleanup_worker_wake(process_t*p){(void)p;worker_wakes++;return true;}
'''
code += 'typedef u64 mg_session_id_t;\n#define PROCESS_NO_SESSION 0U\n#define PROCESS_HANDOFF_RELEASE_BATCH 16U\nstatic void process_reap_session_members(u64 id){assert(id==0);}\n'
for name in ('process_termination_begin', 'process_cleanup_request', 'process_cleanup_worker_reap_once',
             'process_scheduler_post_handoff',
             'process_cleanup_worker_release_handoffs_once'):
    code += namespace['function']('kernel/src/process.c', name) + '\n'
code += r'''
static bool process_cleanup_worker_release_deferred_references_once(void){return false;}
static bool process_cleanup_worker_retry_session_collections_once(void){return false;}
static process_t*process_cleanup_candidate_locked(void){return NULL;}
'''
code += namespace['function']('kernel/src/process.c', 'process_cleanup_worker_run_once') + '\n'
code += r'''
static void dispatch(void){local_thread=&cleanup_thread;
 unsigned loops=0;
 for(;;){assert(loops++<100);if(!process_cleanup_worker_run_once())break;}
 assert(!process_worker_reap_sweep_active);}
static void complete_publish(process_thread_record_t*r,u64 result){
 kernel_thread_t*t=r->execution;process_t*p=r->process;
 process_worker_exit(t,result);t->state=THREAD_STATE_TERMINATED;
 t->running_cpu=THREAD_CPU_NONE;t->context_switch_pending=true;
 assert(object_reference(&p->object));
 assert(process_scheduler_handoff_complete_internal(t,true));
 t->context_switch_pending=false;process_scheduler_post_handoff(p);
}
static void *dispatch_host(void*p){(void)p;dispatch();return NULL;}
int main(void){process_t p;kernel_thread_t initial;
 mutex_init(&vmm_operation_mutex);init(&p,&initial);p.pid=1;all_processes=&p;
 process_handle_t handle;u64 id;
 process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);assert(!worker_wakes&&process_cleanup_work_pending);
 process_cleanup_worker_ready=true;
 /* The producer publishes before worker readiness. No future target syscall
  * is used to drive the real worker consumer and region/record engine. */
 assert(process_thread_create(&p,0x400100,0,&handle,&id)==MG_OK);
 process_thread_record_t*r=p.thread_records;
 assert(process_thread_detach(&p,handle)==MG_OK);complete_publish(r,77);
 assert(p.thread_count==2&&p.user_regions&&worker_wakes);
 process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);
 process_cleanup_worker_ready=true;dispatch();
 assert(p.thread_count==1&&!p.thread_records&&!p.user_regions);
 assert(stack_releases==1&&destroyed_threads==1&&region_releases==2&&!p.worker_reapers);
 /* Retained readers are a real dependency; dispatch must park instead of
  * retrying pending state forever. The last reader event makes it runnable. */
 local_thread=&initial;assert(process_thread_create(&p,0x400100,0,&handle,&id)==MG_OK);r=p.thread_records;
 assert(process_thread_detach(&p,handle)==MG_OK);complete_publish(r,88);p.thread_list_readers=1;
 process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);dispatch();
 assert(p.thread_count==2&&p.thread_records&&p.worker_reap_pending);
 assert(!process_cleanup_work_pending);p.thread_list_readers=0;
 process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);dispatch();assert(p.thread_count==1&&!p.thread_records);
 /* An indefinite region lease cannot occupy the worker. Release and signal
  * a genuine dependency change, then reclaim without another target syscall. */
 local_thread=&initial;assert(process_thread_create(&p,0x400100,0,&handle,&id)==MG_OK);r=p.thread_records;
 assert(process_thread_detach(&p,handle)==MG_OK);complete_publish(r,99);
 lease_on_revoke_kind=PROCESS_USER_REGION_THREAD_TLS;
 process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);dispatch();
 assert(p.thread_records&&p.user_regions&&leased_region&&!p.worker_reapers&&!process_cleanup_work_pending);
 lease_on_revoke_kind=0;leased_region=NULL;
 process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);dispatch();
 assert(!p.thread_records&&!p.user_regions&&p.thread_count==1);
 /* Claim lifetime starts before even provisional-region memory-lock work. */
 local_thread=&initial;assert(process_thread_create(&p,0x400100,0,&handle,&id)==MG_OK);r=p.thread_records;
 assert(process_thread_detach(&p,handle)==MG_OK);complete_publish(r,101);
 p.user_region_reap_pending=true;assert(mutex_lock(&p.memory_lock));
 pthread_t cleaner;block_entered=false;assert(!pthread_create(&cleaner,NULL,dispatch_host,NULL));
 pthread_mutex_lock(&event_lock);while(!block_entered)pthread_cond_wait(&event_changed,&event_lock);pthread_mutex_unlock(&event_lock);
 assert(p.worker_reapers==1&&!process_termination_begin(&p));
 assert(p.state==PROCESS_STATE_ACTIVE&&!p.termination_pending);
 assert(mutex_unlock(&p.memory_lock));pthread_join(cleaner,NULL);
 assert(!p.worker_reapers&&!p.thread_records&&!p.user_regions);
 /* Finite sweeps visit >16 process records without losing continuation work. */
 process_t many[20];kernel_thread_t mains[20];
 for(unsigned i=0;i<20;i++){init(&many[i],&mains[i]);many[i].pid=i+2;many[i].next_all=i?&many[i-1]:&p;
 assert(process_thread_create(&many[i],0x400100,0,&handle,&id)==MG_OK);r=many[i].thread_records;
 assert(process_thread_detach(&many[i],handle)==MG_OK);complete_publish(r,i);}
 all_processes=&many[19];process_cleanup_request(PROCESS_CLEANUP_WORKER_REAP);dispatch();
 for(unsigned i=0;i<20;i++){assert(many[i].thread_count==1&&!many[i].thread_records&&!many[i].user_regions&&!many[i].worker_reapers);clear(&many[i]);}
 /* A generation change at the end of an otherwise empty sweep queues a
  * continuation without a scheduler event. The real run-once must not park. */
 process_worker_reap_sweep_active=true;process_worker_reap_after=100;
 process_worker_reap_limit=100;
 process_worker_reap_seen_generation=process_worker_reap_generation;
 process_worker_reap_generation++;process_cleanup_work_pending=0;
 assert(process_cleanup_worker_run_once());
 assert(process_cleanup_work_pending&PROCESS_CLEANUP_WORKER_REAP);dispatch();
 assert(p.object.ref_count==2);assert(created_threads==24&&destroyed_threads==24&&stack_releases==24&&region_releases==48&&record_destructions==24);clear(&p);puts("autonomous production reaping: pre-ready work, no target syscall, reader/lease deferral, exact resources, and finite >16-process sweeps PASS");return 0;}
'''
# The API fixture's safe kernel continuation is reused as the worker context.
code = code.replace('static void dispatch(void)', 'static kernel_thread_t cleanup_thread;\nstatic void dispatch(void)')
with HostFixture('autonomous-reap') as fixture:
    source = fixture.path / 'autonomous.c'
    binary = fixture.path / 'autonomous'
    source.write_text(code)
    fixture.run([CC, '-std=gnu17', '-O1', '-g', '-pthread', '-Wall', '-Wextra',
                 '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
                 '-fsanitize=address,undefined', str(source), '-o', str(binary)])
    print(fixture.run([str(binary)], env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'}).stdout, end='')
