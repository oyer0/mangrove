#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production scheduler/process handoff ownership under an external collector."""
from pathlib import Path
import ast,re,os,subprocess,sys,tempfile
R=Path(__file__).resolve().parents[2]
CC=sys.argv[1] if len(sys.argv)>1 else 'gcc'
def fn(path,name):
 s=(R/path).read_text()
 for m in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b'+name+r'\s*\(',s,re.M):
  p=s.index('{',m.end())
  if ';' in s[m.end():p]:continue
  end=p+1;depth=1
  while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
  return s[m.start():end]+'\n'
 raise ValueError(name)
def from_text(source,name):
 for m in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b'+name+r'\s*\(',source,re.M):
  p=source.index('{',m.end())
  if ';' in source[m.end():p]:continue
  end=p+1;depth=1
  while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
  return source[m.start():end]+'\n'
 raise ValueError(name)
d={}
for n in ast.parse((R/'tests/kernel/process_thread_lifecycle_test.py').read_text()).body:
 if isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name):
  try:d[n.targets[0].id]=ast.literal_eval(n.value)
  except (ValueError,TypeError):pass
p=d['prefix'].replace('u32 state, running_cpu;', 'uintptr_t saved_stack_pointer,user_fs_base; bool queued; u32 syscall_nesting; u32 state, running_cpu;')
p=p.replace('freed_threads++;return true;', 'freed_threads++;free(thread);return true;')
p=p.replace('static kernel_thread_t *process_cleanup_worker=&cleanup_worker_thread;\nstatic bool process_cleanup_worker_wake(process_t *process)\n{assert(process);cleanup_wake_calls++;return true;}\nstatic void process_reap_session_members(mg_session_id_t session_id)\n{assert(session_id==PROCESS_NO_SESSION);}', 'static kernel_thread_t *process_cleanup_worker=(kernel_thread_t *)1;\nstatic bool process_cleanup_worker_wake(process_t *process)\n{assert(process);cleanup_wake_calls++;return true;}\nstatic void process_reap_session_members(mg_session_id_t session_id)\n{assert(session_id==PROCESS_NO_SESSION);}')
p=re.sub(r'static bool process_cleanup_worker_run_once\(void\);\nstatic bool run_cleanup_worker_once\(process_t \*process\)\n\{.*?\}\n','',p,flags=re.S)
p=p.replace('static void spin_unlock_irqrestore(spinlock_t *lock,u64 flags)', 'static void remote_collect(void);\nstatic bool remote_armed;\nstatic void spin_unlock_irqrestore(spinlock_t *lock,u64 flags)')
p=p.replace('{(void)flags;assert(pthread_mutex_unlock(lock)==0);}', '{(void)flags;assert(pthread_mutex_unlock(lock)==0); if(lock==&scheduler_lock&&remote_armed){remote_armed=false;remote_collect();}}')
p+='\nstatic bool process_cleanup_exit_resources(process_t *process)\n{(void)process;return false;}\n'
p+=r'''
#define MSR_IA32_FS_BASE 0xc0000100U
#define THREAD_CPU_NONE UINT32_MAX
#define current_thread local_thread
static bool scheduler_context_switch_in_progress;
static kernel_thread_t bootstrap_thread;
typedef struct {kernel_thread_t *scheduler_idle_thread;} cpu_local_t;
static cpu_local_t cpu;
static cpu_local_t *scheduler_cpu_local(void){return &cpu;}
static cpu_local_t *cpu_by_index(u32 i){return i==0?&cpu:NULL;}
static u32 cpu_count(void){return 1;}
static void wrmsr(u32 msr,u64 value){(void)msr;(void)value;}
static void fpu_state_restore(unsigned *state){(void)state;}
static bool scheduler_enqueue_locked(kernel_thread_t *t){(void)t;return true;}
static void scheduler_remove_queued_locked(kernel_thread_t*t){(void)t;}
static void sleeping_remove(kernel_thread_t*t){(void)t;}
static void mutex_cancel_waiter(kernel_thread_t*t){(void)t;}
static struct {unsigned sleeping_threads,blocked_threads;} scheduler_stats;
static process_t *process_current(void){return local_thread?local_thread->process:NULL;}
static bool scheduler_terminate_thread_group(kernel_thread_t *,usize);
'''
process_functions=(
 'process_storage_authorization_clear_locked',
 'process_thread_register_locked',
 'process_thread_list_complete_locked',
 'process_thread_list_read_acquire_locked',
 'process_thread_list_read_release',
 'process_waiter_wake_release',
 'process_publish_terminated_locked',
 'process_attach_thread',
 'process_detach_thread_owned',
 'process_finish_termination',
 'process_exit',
 'process_scheduler_handoff_complete_internal',
 'process_scheduler_handoff_complete',
 'process_scheduler_context_handoff_complete',
 'process_scheduler_post_handoff',
 'process_finalize_exit')
functions=''.join(fn('kernel/src/process.c',name) for name in process_functions)
functions+=''.join(fn('kernel/src/process.c',name) for name in ('process_termination_begin','process_termination_cancel','process_terminate_external'))
functions+=fn('kernel/src/scheduler.c','scheduler_terminate_thread_group')
functions+=fn('kernel/src/scheduler.c','scheduler_context_switch_complete')
functions+=fn('kernel/src/scheduler.c','scheduler_context_switch_post_complete')
suffix=d['suffix'].split('int main(void)')[0]
helper=''.join(from_text(suffix,name) for name in ('thread_running','reset_process'))
main=r'''
static process_t *target;
static kernel_thread_t *initial_thread;
static void remote_collect(void) {
 /* CPU1 executes the actual external termination path at the first scheduler
  * unlock, while CPU0 still owns the context-switch callback barrier. */
 assert(!process_terminate_external(target,-1));
 assert(freed_threads==0 && initial_thread->process==target &&
        initial_thread->context_switch_pending);
 fprintf(stderr,"external collector refused to detach callback-owned descriptor\n");
}
int main(void) {
 process_t process;kernel_thread_t unused,kernel_incoming={0};
 kernel_thread_t *initial=calloc(1,sizeof(*initial));assert(initial);
 reset_process(&process,initial,&unused);
 assert(process_attach_thread(&process,initial));
 thread_running(initial,0);
 local_thread=initial;assert(process_exit(&process,0));
 initial->state=THREAD_STATE_TERMINATED;initial->running_cpu=0;
 initial->context_switch_pending=true;
 local_thread=&kernel_incoming;target=&process;initial_thread=initial;
 remote_armed=true;
 process_t *handoff=scheduler_context_switch_complete(
     &initial->saved_stack_pointer);
 assert(handoff==&process && freed_threads==0 &&
        !initial->context_switch_pending &&
        initial->process_state==THREAD_PROCESS_HANDOFF_COMPLETE &&
        process.live_thread_count==0);
 /* The process reference lives through the stack-local post hook. Only after
  * the callback has returned can cleanup/termination/final reap destroy the
  * outgoing descriptor. */
 scheduler_context_switch_post_complete(handoff);
 assert(process.scheduler_handoff_references==1 &&
        __atomic_load_n(&process.scheduler_handoff_releases_pending,
                        __ATOMIC_ACQUIRE)==1 &&
        process.object.ref_count==3);
 process.exit_cleanup_complete=true;
 assert(process_finish_termination(&process));
 assert(process_finalize_exit(&process));
 assert(freed_threads==1 && process.thread_count==0 &&
        process.object.ref_count==2 &&
        process.scheduler_handoff_references==1 &&
        __atomic_load_n(&process.scheduler_handoff_releases_pending,
                        __ATOMIC_ACQUIRE)==1);
 puts("scheduler handoff lifetime: external collector race deferred safely PASS");
 return 0;
}
'''
code=p+functions+helper+main
with tempfile.TemporaryDirectory(prefix='mangrove-handoff-lifetime-') as folder:
 folder=Path(folder);source=folder/'handoff_lifetime.c';binary=folder/'handoff_lifetime'
 source.write_text(code)
 env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}
 subprocess.run([CC,'-std=gnu17','-g','-O1','-Wall','-Wextra',
     '-Wno-unused-function','-Wno-unused-variable','-Werror',
     '-fsanitize=address,undefined','-pthread',str(source),'-o',str(binary)],
     check=True,env=env)
 subprocess.run([str(binary)],check=True,env=env,timeout=10)
