#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Focused production-source tests for shutdown ordering and VFS syncing.

These host fixtures exercise the real lifecycle functions with deterministic
process, scheduler, and storage callbacks. They do not emulate ACPI hardware.
"""
from pathlib import Path
import re
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


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
    raise ValueError(f"missing production function {path}:{name}")


def run(name, source):
    with tempfile.TemporaryDirectory(prefix=f"mangrove-{name}-") as tmp:
        path = Path(tmp) / f"{name}.c"
        binary = Path(tmp) / name
        path.write_text(source)
        subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(path), "-o",
                        str(binary)], check=True)
        env = os.environ.copy()
        # LeakSanitizer cannot initialize reliably in this guest/container
        # environment; retain AddressSanitizer and UBSan coverage.
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=env)


process_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t i32;
enum { PROCESS_STATE_ACTIVE = 1, PROCESS_STATE_TERMINATED = 2 };
enum { MG_SERVICE_SPROUT = 1, MG_SERVICE_SESSIOND = 2,
       MG_SERVICE_LOGIND = 3, MG_SERVICE_LEAF = 4 };
enum { MG_OK = 0, MG_ERR_BAD_ARGUMENT = -4, MG_ERR_BUSY = -14,
       MG_ERR_TIMEOUT = -18 };
#define PROCESS_SHUTDOWN_GRACE_MS 5000U
#define PROCESS_SHUTDOWN_FORCE_MS 5000U
typedef struct { unsigned references; } kernel_object_t;
typedef struct kernel_thread { bool pending_wake,running,exit_pending; } kernel_thread_t;
typedef struct process process_t;
struct process {
 int state; bool system_service, shutdown_requested, shutdown_wake_sent;
 bool launch_ready;
 u32 service_id; u64 pid; char name[32]; kernel_thread_t *main_thread;
 kernel_object_t object; process_t *next_all;
};
static process_t *all_processes;
static u32 process_registry_lock;
static bool process_shutdown_active;
static u64 now_ms;
static bool cooperative_exit, terminate_allowed;
static unsigned wake_calls, terminate_calls, sleep_calls;
static process_t *requester;
static bool app_state_active_for_test=true;
static u64 spin_lock_irqsave(u32 *lock) { assert(!*lock); *lock=1; return 0; }
static void spin_unlock_irqrestore(u32 *lock,u64 flags)
{ (void)flags; assert(*lock); *lock=0; }
static bool object_reference(kernel_object_t *object)
{ if (!object || !object->references) return false; object->references++; return true; }
static void object_release(kernel_object_t *object)
{ assert(object && object->references > 1); object->references--; }
static bool scheduler_wake_waiter(kernel_thread_t *thread)
{ assert(thread); wake_calls++; thread->pending_wake=true; return true; }
static bool scheduler_thread_exit_complete(const kernel_thread_t *thread)
{return thread && !thread->running && !thread->exit_pending;}
static bool process_terminate_external(process_t *process, i32 status)
{ (void)status; terminate_calls++; if (!terminate_allowed) return false;
 process->state=PROCESS_STATE_TERMINATED; return true; }
static u64 timer_uptime_ms(void) { return now_ms; }
static bool scheduler_sleep(u64 ms)
{ (void)ms; sleep_calls++; now_ms += 1000;
 if (cooperative_exit) for (process_t *p=all_processes; p; p=p->next_all)
   if (p != requester && p->state==PROCESS_STATE_TERMINATED &&
       p->main_thread && (p->main_thread->running || p->main_thread->exit_pending)) {
     p->main_thread->running=false;p->main_thread->exit_pending=false;break;
   }
 if (cooperative_exit) for (process_t *p=all_processes; p; p=p->next_all)
   if (p != requester && p->shutdown_requested &&
       p->state == PROCESS_STATE_ACTIVE) {
     if (p->system_service) assert(!app_state_active_for_test);
     else app_state_active_for_test=false;
     p->state=PROCESS_STATE_TERMINATED; break;
   }
 return true; }
static void kprint(const char *format, ...) { (void)format; }
static bool process_matches_shutdown_phase(const process_t *,const process_t *,bool);
static bool process_in_shutdown_phase(const process_t *,const process_t *,bool);
static void process_shutdown_mark_phase(process_t *,bool);
static process_t *process_shutdown_next_work(process_t *,bool);
static int process_shutdown_wait_phase(process_t *,bool);
static void process_shutdown_abort(void);
'''
process_functions = "".join(function("kernel/src/process.c", name) for name in (
    "process_shutdown_requested", "process_matches_shutdown_phase",
    "process_in_shutdown_phase", "process_shutdown_mark_phase",
    "process_shutdown_next_work",
    "process_shutdown_wait_phase", "process_shutdown_quiesce",
    "process_shutdown_abort", "process_shutdown_pending"))
process_suffix = r'''
static void init_process(process_t *p,u64 pid,const char *name,bool service,
                         u32 service_id,kernel_thread_t *thread)
{ memset(p,0,sizeof(*p)); p->state=PROCESS_STATE_ACTIVE; p->pid=pid;
  strncpy(p->name,name,sizeof(p->name)-1); p->system_service=service;
  p->service_id=service_id; p->main_thread=thread; p->object.references=1;
  p->launch_ready=true; }
int main(void)
{
 process_t caller,app,leaf,sprout,sessiond,logind;
 kernel_thread_t caller_thread={0},app_thread={0},leaf_thread={0};
 init_process(&caller,20,"shutdown",false,0,&caller_thread); requester=&caller;
 init_process(&app,21,"shoot",false,0,&app_thread);
 init_process(&leaf,22,"logd",true,MG_SERVICE_LEAF,&leaf_thread);
 init_process(&sprout,1,"sprout",true,MG_SERVICE_SPROUT,NULL);
 init_process(&sessiond,2,"sessiond",true,MG_SERVICE_SESSIOND,NULL);
 init_process(&logind,3,"logind",true,MG_SERVICE_LOGIND,NULL);
 caller.next_all=&app; app.next_all=&leaf; leaf.next_all=&sprout;
 sprout.next_all=&sessiond; sessiond.next_all=&logind; all_processes=&caller;
 cooperative_exit=true;app_state_active_for_test=true;
 assert(process_shutdown_quiesce(&caller)==MG_OK);
 assert(process_shutdown_pending());
 assert(app.state==PROCESS_STATE_TERMINATED && leaf.state==PROCESS_STATE_TERMINATED);
 assert(!app_state_active_for_test);
 assert(sprout.state==PROCESS_STATE_ACTIVE && sessiond.state==PROCESS_STATE_ACTIVE &&
        logind.state==PROCESS_STATE_ACTIVE && caller.state==PROCESS_STATE_ACTIVE);
 assert(app_thread.pending_wake && leaf_thread.pending_wake && wake_calls==2);
 assert(app.object.references==1 && leaf.object.references==1);
 process_shutdown_abort();
 assert(!process_shutdown_pending() && !app.shutdown_requested &&
        !leaf.shutdown_requested && !caller.shutdown_requested);

 /* TERMINATED is published before the final exit syscall returns. Shutdown
  * must wait for that live frame to finish before filesystem sync. */
 all_processes=&caller;caller.next_all=&app;app.next_all=NULL;
 app.state=PROCESS_STATE_TERMINATED;app.shutdown_requested=false;
 app_thread.running=true;cooperative_exit=true;app_state_active_for_test=true;
 wake_calls=0;now_ms=0;process_shutdown_active=false;
 assert(process_shutdown_quiesce(&caller)==MG_OK);
 assert(!app_thread.running && wake_calls==0 && app.object.references==1);
 process_shutdown_abort();

 /* A non-running thread can still be suspended inside exit cleanup. */
 app.state=PROCESS_STATE_TERMINATED;app_thread.running=false;
 app_thread.exit_pending=true;now_ms=0;
 assert(process_shutdown_quiesce(&caller)==MG_OK);
 assert(now_ms>0 && !app_thread.exit_pending);
 process_shutdown_abort();

 /* A cleanup frame that never completes cancels shutdown, even off-CPU. */
 app_thread.exit_pending=true;cooperative_exit=false;terminate_allowed=false;
 now_ms=0;
 assert(process_shutdown_quiesce(&caller)==MG_ERR_TIMEOUT);
 assert(app_thread.exit_pending && !process_shutdown_pending());
 app_thread.exit_pending=false;

 /* Safe forced termination is attempted only after the cooperative grace. */
 app.state=PROCESS_STATE_ACTIVE; app.shutdown_requested=false;
 all_processes=&caller; caller.next_all=&app; app.next_all=NULL;
 cooperative_exit=false; app_state_active_for_test=true;
 terminate_allowed=true; terminate_calls=0;
 now_ms=0; sleep_calls=0; process_shutdown_active=false;
 assert(process_shutdown_quiesce(&caller)==MG_OK);
 assert(app.state==PROCESS_STATE_TERMINATED && terminate_calls==1 && now_ms>=5000);
 process_shutdown_abort();

 /* A process stuck in an unsafe syscall produces a bounded timeout and
  * clears the admission gate; it is never reported as clean shutdown. */
 app.state=PROCESS_STATE_ACTIVE; app.shutdown_requested=false;
 app_state_active_for_test=true; terminate_allowed=false;
 terminate_calls=0; now_ms=0; sleep_calls=0;
 assert(process_shutdown_quiesce(&caller)==MG_ERR_TIMEOUT);
 assert(terminate_calls>0 && now_ms>=10000 && !process_shutdown_pending());
 assert(!app.shutdown_requested && app.state==PROCESS_STATE_ACTIVE);
 puts("shutdown process lifecycle: cooperative ordering, safe forced termination, race-safe waiter wake, and bounded timeout passed");
 return 0;
}
'''

vfs_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64; typedef uint32_t u32;
typedef u32 mutex_t;
enum {VFS_OK=0,VFS_ERR_IO=-1,VFS_ERR_DEVICE_GONE=-2,
      VFS_ERR_UNCERTAIN=-3,VFS_ERR_BUSY=-4,VFS_ERR_NO_MEM=-5};
enum {VFS_SUPER_ACTIVE=1,VFS_SUPER_DETACHING=2,VFS_SUPER_DEAD=3};
#define VFS_MAX_MOUNTS 4U
#define VFS_SHUTDOWN_LOCK_TIMEOUT_MS 10000U
typedef struct vfs_super vfs_super_t;
typedef struct {int (*sync)(vfs_super_t *);} vfs_ops_t;
struct vfs_super {u32 state,reference_count,operation_lock; const vfs_ops_t *ops;
 void *dev; u64 device_id;};
typedef struct {bool active;vfs_super_t *sb;} vfs_mount_t;
static u32 vfs_mount_lock,vfs_metadata_lock;
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static bool fail_mount_lock,fail_operation_lock,fail_flush;
static unsigned hook_calls,flush_calls;
static u64 now_ms;
static vfs_super_t *expected_a,*expected_b;
static u64 spin_lock_irqsave(u32 *lock){assert(!*lock);*lock=1;return 0;}
static void spin_unlock_irqrestore(u32 *lock,u64 flags)
{(void)flags;assert(*lock);*lock=0;}
static bool mutex_try_lock(u32 *lock)
{if(lock==&vfs_mount_lock && fail_mount_lock)return false;
 if(lock!=&vfs_mount_lock && fail_operation_lock)return false;
 assert(!*lock);*lock=1;return true;}
static bool mutex_unlock(u32 *lock){assert(*lock);*lock=0;return true;}
static u64 timer_uptime_ms(void){return now_ms;}
static bool scheduler_sleep(u64 ms)
{assert(ms==1U);now_ms+=1000U;return true;}
static bool vfs_super_is_live_locked(vfs_super_t *sb)
{return sb && sb->state!=VFS_SUPER_DEAD;}
static void vfs_super_release(vfs_super_t *sb)
{assert(sb && sb->reference_count>1);sb->reference_count--;}
static bool block_flush_for_id(void *dev,u64 id)
{(void)dev;(void)id;flush_calls++;
 assert(expected_a->state==VFS_SUPER_DETACHING &&
        expected_b->state==VFS_SUPER_DETACHING);return !fail_flush;}
static int sync_hook(vfs_super_t *sb)
{hook_calls++;assert(sb->state==VFS_SUPER_DETACHING &&
 expected_a->state==VFS_SUPER_DETACHING &&
 expected_b->state==VFS_SUPER_DETACHING);
 assert(expected_a->reference_count==2 && expected_b->reference_count==2);
 return fail_flush?VFS_ERR_IO:VFS_OK;}
static const vfs_ops_t hook_ops={sync_hook};
'''
vfs_function = (function("kernel/src/vfs.c", "vfs_sync_lock_before") +
                function("kernel/src/vfs.c", "vfs_sync_all"))
vfs_suffix = r'''
int main(void)
{
 vfs_super_t a={.state=VFS_SUPER_ACTIVE,.reference_count=1,.ops=&hook_ops};
 vfs_super_t b={.state=VFS_SUPER_ACTIVE,.reference_count=1,.dev=(void*)1,
                .device_id=77};
 expected_a=&a;expected_b=&b;
 mount_table[0]=(vfs_mount_t){true,&a};mount_table[1]=(vfs_mount_t){true,&a};
 mount_table[2]=(vfs_mount_t){true,&b};
 assert(vfs_sync_all()==VFS_OK);
 assert(hook_calls==1 && flush_calls==1 && a.state==VFS_SUPER_ACTIVE &&
        b.state==VFS_SUPER_ACTIVE && a.reference_count==1 &&
        b.reference_count==1 && !vfs_mount_lock && !vfs_metadata_lock &&
        !a.operation_lock && !b.operation_lock);
 fail_flush=true;
 assert(vfs_sync_all()==VFS_ERR_IO);
 assert(hook_calls==2 && a.state==VFS_SUPER_ACTIVE &&
        b.state==VFS_SUPER_ACTIVE && a.reference_count==1 &&
        b.reference_count==1 && !vfs_mount_lock);
 fail_flush=false;fail_operation_lock=true;now_ms=0;
 assert(vfs_sync_all()==VFS_ERR_BUSY && now_ms>=10000U);
 assert(a.state==VFS_SUPER_ACTIVE && b.state==VFS_SUPER_ACTIVE &&
        a.reference_count==1 && b.reference_count==1 && !vfs_mount_lock);
 fail_operation_lock=false;fail_mount_lock=true;now_ms=0;
 assert(vfs_sync_all()==VFS_ERR_BUSY && now_ms>=10000U &&
        a.reference_count==1 && b.reference_count==1);
 puts("VFS shutdown sync: unique mounts, flush, rollback, reference release, and bounded lock timeouts passed");
 return 0;
}
'''

power_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int64_t i64;typedef int32_t i32;
enum {MG_OK=0,MG_ERR_BUSY=-14,MG_ERR_BAD_ARGUMENT=-4,
      MG_ERR_DEVICE_GONE=-31,MG_ERR_UNCERTAIN=-33,
      MG_ERR_NO_MEMORY=-6,MG_ERR_IO=-12};
enum {VFS_OK=0,VFS_ERR_DEVICE_GONE=-2,VFS_ERR_UNCERTAIN=-3,
      VFS_ERR_BUSY=-4,VFS_ERR_NO_MEM=-5};
typedef struct process {int unused;} process_t;
static process_t caller;
static int quiesce_result,sync_result,abort_calls;
static int trace[4],trace_count;
static process_t *process_current(void){return &caller;}
static int process_shutdown_quiesce(process_t *p)
{assert(p==&caller);trace[trace_count++]=1;return quiesce_result;}
static int vfs_sync_all(void){trace[trace_count++]=2;return sync_result;}
static void process_shutdown_abort(void){abort_calls++;trace[trace_count++]=3;}
static void kprint(const char *format,...){(void)format;}
'''
power_functions = "".join(function("kernel/src/platform_power.c", name)
                        for name in ("power_sync_result", "power_quiesce_and_sync"))
power_suffix = r'''
int main(void)
{
 quiesce_result=MG_ERR_BUSY;assert(power_quiesce_and_sync()==MG_ERR_BUSY);
 assert(trace_count==1 && trace[0]==1 && abort_calls==0);
 trace_count=0;quiesce_result=MG_OK;sync_result=VFS_ERR_UNCERTAIN;
 assert(power_quiesce_and_sync()==-33);
 assert(trace_count==3 && trace[0]==1 && trace[1]==2 && trace[2]==3 &&
        abort_calls==1);
 trace_count=0;sync_result=VFS_OK;assert(power_quiesce_and_sync()==MG_OK);
 assert(trace_count==2 && trace[0]==1 && trace[1]==2 && abort_calls==1);
 puts("power transition gate: quiesce precedes filesystem sync; failures abort before transition");
 return 0;
}
'''

logind_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;typedef int mg_result_t;
enum {MG_OK=0};
static bool pending_values[4];static int result_values[4];
static unsigned pending_index,sleep_count;
static int system_shutdown_pending(bool *pending)
{unsigned i=pending_index++;*pending=pending_values[i];return result_values[i];}
static int process_sleep_ms(u64 milliseconds)
{assert(milliseconds==50U);sleep_count++;return MG_OK;}
'''
logind_function = function("userspace/logind/main.c",
                           "wait_for_shutdown_resolution")
logind_suffix = r'''
int main(void)
{
 pending_values[0]=true;pending_values[1]=true;pending_values[2]=false;
 wait_for_shutdown_resolution();assert(pending_index==3 && sleep_count==2);
 pending_index=sleep_count=0;pending_values[0]=true;result_values[0]=-1;
 pending_values[1]=false;result_values[1]=MG_OK;
 wait_for_shutdown_resolution();assert(pending_index==2 && sleep_count==1);
 puts("logind shutdown wait: session/login control plane remains idle until transition or cancellation");
 return 0;
}
'''

run("shutdown-process", process_prefix + process_functions + process_suffix)
run("shutdown-vfs", vfs_prefix + vfs_function + vfs_suffix)
run("shutdown-power", power_prefix + power_functions + power_suffix)
run("shutdown-logind", logind_prefix + logind_function + logind_suffix)

logind_source = (ROOT / "userspace/logind/main.c").read_text()
wait_after_shell = logind_source.index(
    "wait_for_shutdown_resolution();",
    logind_source.index("result = process_wait(session.shell"))
retire_after_wait = logind_source.index(
    "retire_session_before_login(session.id, identity.username);",
    wait_after_shell)
retire_helper = function("userspace/logind/main.c",
                         "retire_session_before_login")
assert wait_after_shell < retire_after_wait
assert "sessiond_end(session_id)" in retire_helper
assert logind_source.count("wait_for_shutdown_resolution();") >= 2
sprout_source = (ROOT / "userspace/sprout/main.c").read_text()
assert "if (system_shutdown_pending(&shutdown_pending) != MG_OK)\n            shutdown_pending = true;" in sprout_source
assert sprout_source.index("if (!shutdown_pending)") < sprout_source.index("service_tick(&service_runtime[index])")

# Both public transition paths must pass through the tested lifecycle gate
# before issuing their terminal ACPI operation.
power_source = (ROOT / "kernel/src/platform_power.c").read_text()
for name, terminal_operation in (("platform_poweroff", "power_register_write"),
                                 ("platform_reboot", "power_register_write")):
    body = function("kernel/src/platform_power.c", name)
    assert body.index("power_quiesce_and_sync()") < body.rindex(terminal_operation)

print("poweroff and reboot source ordering: lifecycle gate precedes terminal ACPI writes")
