#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual session state machine and process guards; image/scheduling are modeled."""
from pathlib import Path
import os
import re
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/shared'))
from host_fixture import HostFixture
CC=sys.argv[1] if len(sys.argv)>1 else 'gcc'

def fn(name):
    s=(ROOT/'kernel/src/process.c').read_text()
    for m in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b'+name+r'\s*\(',s,re.M):
        b=s.index('{',m.end())
        if ';' in s[m.end():b]:continue
        e=b+1;d=1
        while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
        return s[m.start():e]+'\n'
    raise ValueError(name)
prefix=r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "process_cleanup.h"
typedef uint64_t u64;typedef uint32_t u32;typedef int32_t i32;
typedef uint64_t mg_session_id_t;typedef uint32_t process_handle_t;
typedef u32 mg_service_id_t;typedef int mg_session_state_t,mg_session_activity_t;
enum {MG_OK=0,MG_ERR_NOT_FOUND=-2,MG_ERR_BUSY=-14,MG_ERR_SHUTDOWN=-38,
 MG_ERR_BAD_ARGUMENT=-4,MG_ERR_PRIVILEGE_REQUIRED=-25,MG_ERR_AUTH_FAILED=-26,
 MG_ERR_ACCESS_DENIED=-13,MG_ERR_INVALID_EXEC=-21};
enum {MG_SESSION_STATE_CREATING=1,MG_SESSION_STATE_ACTIVE,MG_SESSION_STATE_ENDING,MG_SESSION_STATE_ENDED};
enum {MG_SESSION_ACTIVITY_INACTIVE,MG_SESSION_ACTIVITY_ACTIVE};
enum {MG_SERVICE_SESSIOND=2,MG_SERVICE_LOGIND=3,MG_SERVICE_CLIPD=8};
enum {PROCESS_STATE_ACTIVE,PROCESS_STATE_EXITING,PROCESS_STATE_TERMINATED};
enum {THREAD_PROCESS_ATTACHED,THREAD_PROCESS_EXIT_REQUESTED};
#define IDENTITY_PRIVILEGE_MANAGE_SESSIONS 1
#define PROCESS_NO_SESSION 0U
typedef pthread_mutex_t spinlock_t;
typedef struct process process_t;
typedef struct {u32 refs;} kernel_object_t;
typedef struct kernel_thread {u64 id;process_t *process;struct kernel_thread*process_next;
 int process_state;bool process_exit_requested;}kernel_thread_t;
typedef struct {u32 uid,role;char username[32],home[64];} user_identity_t;
typedef user_identity_t mg_identity_t;
typedef struct {u32 privilege;}process_credentials_t;
struct process {u64 pid,session_id;u32 service_id;int state;bool system_service,
 launch_ready,termination_pending,exit_requested,external_termination_in_progress;
 u32 thread_list_readers;kernel_thread_t*threads;process_t*next_all;
 i32 exit_status;kernel_object_t object;bool dynamic;};
typedef struct {process_t*child;process_handle_t handle;u64 pid,attempt;}process_session_launch_t;
typedef struct {mg_session_id_t id;process_handle_t shell;}mg_session_info_t;
typedef struct {u64 id;u32 uid,role,state,activity;char username[32];}mg_session_status_t;
static spinlock_t process_registry_lock=PTHREAD_MUTEX_INITIALIZER;
static process_t *all_processes;
static process_t frontend,backend,clipboard;
static kernel_thread_t initial;
static _Thread_local kernel_thread_t *self;
static _Thread_local unsigned lock_depth;
static _Thread_local bool irq=true;
static _Thread_local bool activation_pause,creation_return_pause;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned pause_stage,fail_stage;
static bool paused,resume_launch;
static unsigned children,destructions,wakes,cancel_wakes,retire_calls;
static u64 next_child=100;
static unsigned process_cleanup_work_pending;
static u64 process_worker_reap_generation;
static bool process_cleanup_worker_ready=true;
static bool process_cleanup_worker_wake(process_t*p){(void)p;__atomic_fetch_add(&wakes,1,__ATOMIC_RELAXED);return true;}
static void pause_here(unsigned stage){pthread_mutex_lock(&gate);
 if(pause_stage==stage){paused=true;pthread_cond_broadcast(&changed);
 while(!resume_launch)pthread_cond_wait(&changed,&gate);}
 pthread_mutex_unlock(&gate);}
static u64 spin_lock_irqsave(spinlock_t*l){u64 old=irq;irq=false;
 assert(!pthread_mutex_lock(l));lock_depth++;return old;}
static void spin_unlock_irqrestore(spinlock_t*l,u64 flags){assert(lock_depth);lock_depth--;
 assert(!pthread_mutex_unlock(l));irq=flags!=0;
 /* Activation pause is after session unlock but before registry unlock. */
 if(activation_pause&&l!=&process_registry_lock){activation_pause=false;pause_here(3);}
 if(creation_return_pause&&l==&process_registry_lock){creation_return_pause=false;pause_here(4);}}
static void spinlock_init(spinlock_t*l){assert(!pthread_mutex_init(l,NULL));}
static void panic(const char*s){fprintf(stderr,"panic %s\n",s);abort();}
static kernel_thread_t*thread_current(void){return self;}
static bool process_get_credentials(process_t*p,process_credentials_t*c)
{if(!p)return false;c->privilege=1;return true;}
static bool identity_credentials_has_privilege(const process_credentials_t*c,u32 p){(void)p;return c->privilege;}
static bool identity_registry_lookup_username(const char*n,user_identity_t*out){
 if(!strcmp(n,"missing"))return false;
 *out=(user_identity_t){.uid=!strcmp(n,"first")?11:22,.role=1};
 strcpy(out->username,n);strcpy(out->home,"/home/test");return true;}
static bool identity_registry_autologin_user(user_identity_t*out){return identity_registry_lookup_username("first",out);}
static bool identity_credentials_from_user(const user_identity_t*i,process_credentials_t*c){(void)i;c->privilege=1;return true;}
static bool ipc_request_context_origin(process_t*b,process_handle_t h,u32 service,u64*out)
{assert(b==&backend&&h==1&&service==MG_SERVICE_LOGIND);*out=frontend.pid;return true;}
static bool process_cleanup_context_valid(void){return irq&&!lock_depth;}
static bool process_thread_list_read_acquire_locked(process_t*p){assert(lock_depth&&p->threads);p->thread_list_readers++;return true;}
static void process_thread_list_read_release(process_t*p){assert(!lock_depth&&p->thread_list_readers);p->thread_list_readers--;}
static bool object_reference(kernel_object_t*o){assert(o->refs);o->refs++;return true;}
static void process_storage_authorization_clear_locked(process_t*p){assert(lock_depth&&p);}
static void console_cancel_waiter(kernel_thread_t*t){assert(!lock_depth&&t->process->thread_list_readers);cancel_wakes++;}
static bool scheduler_wake_waiter(kernel_thread_t*t){assert(!lock_depth&&t->process->thread_list_readers);return true;}
static void release_child(process_t*p){assert(irq&&!lock_depth&&p->object.refs);p->object.refs--;
 if(!p->object.refs){assert(p->dynamic&&!p->thread_list_readers&&p->state==PROCESS_STATE_TERMINATED);
  destructions++;free(p->threads);free(p);}}
static void process_handle_reference_release(kernel_object_t*o){
 process_t*p=(process_t*)((char*)o-__builtin_offsetof(process_t,object));release_child(p);}
static void process_reap_session_members(u64 id){assert(!lock_depth&&irq);
 u64 f=spin_lock_irqsave(&process_registry_lock);process_t**at=&all_processes;
 while(*at){process_t*p=*at;if(p->dynamic&&p->session_id==id&&p->state==PROCESS_STATE_TERMINATED&&p->object.refs==1){
  *at=p->next_all;spin_unlock_irqrestore(&process_registry_lock,f);release_child(p);
  f=spin_lock_irqsave(&process_registry_lock);at=&all_processes;
 }else at=&p->next_all;}
 spin_unlock_irqrestore(&process_registry_lock,f);}
int process_activate_session(process_t*,u64,const user_identity_t*,u64*);
int session_publish_active(u64,const user_identity_t*,u64*);
bool session_commit_launch(u64,u64,u64,u64,u64);
bool process_retire_session_members(u64);
int process_spawn_session_shell(process_t*,const process_credentials_t*,u64,u64,const char*,process_session_launch_t*);
'''
source=(ROOT/'kernel/src/session.c').read_text()
source=re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
code=prefix+source+'\n'+fn('process_activate_session')+fn('process_session_launch_permit')
retire=fn('process_retire_session_members').replace('    if (!session_id) return false;', '    retire_calls++;if (!session_id) return false;')
code+=retire+fn('process_cleanup_request')
code+=r'''
/* This boundary models image preparation/enqueue only. The arguments fixture
 * executes the full production preparation engine with allocation failures. */
int process_spawn_session_shell(process_t*p,const process_credentials_t*c,u64 id,
 u64 attempt,const char*cwd,process_session_launch_t*out){(void)c;(void)cwd;
 assert(!lock_depth&&irq);pause_here(1);
 if(fail_stage==1)return MG_ERR_INVALID_EXEC;
 process_t*child=calloc(1,sizeof(*child));assert(child);
 child->dynamic=true;child->pid=next_child++;child->state=PROCESS_STATE_ACTIVE;
 child->session_id=id;child->launch_ready=false;child->object.refs=3;
 child->threads=calloc(1,sizeof(kernel_thread_t));assert(child->threads);
 child->threads->process=child;
 u64 f=spin_lock_irqsave(&process_registry_lock);child->next_all=all_processes;all_processes=child;
 spin_unlock_irqrestore(&process_registry_lock,f);
 if(!process_session_launch_permit(p,id,attempt,child->pid)||fail_stage==2){
  f=spin_lock_irqsave(&process_registry_lock);assert(all_processes==child);all_processes=child->next_all;
  spin_unlock_irqrestore(&process_registry_lock,f);free(child->threads);free(child);return MG_ERR_SHUTDOWN;}
 children++;pause_here(2);child->launch_ready=true;
 if(fail_stage==3)child->state=PROCESS_STATE_TERMINATED; /* Early worker exit. */
 *out=(process_session_launch_t){.child=child,.handle=(u32)child->pid,.pid=child->pid,.attempt=attempt};return MG_OK;}
static unsigned dispatch(void){unsigned passes=0;
 for(;;){unsigned work=__atomic_exchange_n(&process_cleanup_work_pending,0,__ATOMIC_ACQ_REL);
  if(!work)break;
  assert(++passes<20);
  if(work&PROCESS_CLEANUP_SESSION_PROGRESS)session_cleanup_progress();}
 return passes;}
static u64 create(const char*name){mg_session_info_t info={0};
 assert(session_create_process(&backend,1,name,&info)==MG_OK&&info.id&&!info.shell);return info.id;}
static session_record_t*find(u64 id){return session_find(id);}
static void ready(void){paused=false;resume_launch=false;fail_stage=pause_stage=0;}
static void wait_pause(void){pthread_mutex_lock(&gate);while(!paused)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);}
static void release_pause(void){pthread_mutex_lock(&gate);resume_launch=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);}
struct call {u64 id;int result;u32 handle;};
static void*launch_host(void*raw){struct call*c=raw;kernel_thread_t creator={.id=17,.process=&frontend};self=&creator;
 c->result=session_launch_process(&frontend,c->id,&c->handle);return NULL;}
static void*activate_host(void*raw){u64*out=raw;activation_pause=pause_stage==3;creation_return_pause=pause_stage==4;*out=create("first");return NULL;}
static void finish_member(u64 id){u64 f=spin_lock_irqsave(&process_registry_lock);
 for(process_t*p=all_processes;p;p=p->next_all)if(p->dynamic&&p->session_id==id){
  p->state=PROCESS_STATE_TERMINATED;assert(p->object.refs==2);p->object.refs--; /* Close external handle. */}
 spin_unlock_irqrestore(&process_registry_lock,f);process_cleanup_request(PROCESS_CLEANUP_SESSION_PROGRESS);}
int main(void){frontend=(process_t){.pid=7,.state=PROCESS_STATE_ACTIVE,.launch_ready=true,.system_service=true,.service_id=MG_SERVICE_LOGIND};
 backend=(process_t){.pid=8,.state=PROCESS_STATE_ACTIVE,.launch_ready=true,.system_service=true,.service_id=MG_SERVICE_SESSIOND};
 clipboard=(process_t){.pid=9,.system_service=true,.service_id=MG_SERVICE_CLIPD};
 frontend.next_all=&backend;all_processes=&frontend;initial=(kernel_thread_t){.id=16,.process=&frontend};self=&initial;
 session_init();u64 id=create("first");assert(find(id)->identity.uid==11);mg_session_status_t status;
 assert(session_query_process(&clipboard,id,&status)==MG_OK&&status.uid==11);
 assert(session_end_process(&clipboard,id)==MG_ERR_PRIVILEGE_REQUIRED);
 /* Atomic identity creation, bounded table and no reused ID after slot reuse. */
 u64 ids[8]={id};for(unsigned i=1;i<8;i++)ids[i]=create("other");
 mg_session_info_t full;assert(session_create_process(&backend,1,"first",&full)==MG_ERR_BUSY);
 for(unsigned i=0;i<8;i++)assert(session_end_process(&backend,ids[i])==MG_ERR_BUSY);
 dispatch();for(unsigned i=0;i<8;i++)assert(!find(ids[i]));
 id=create("first");assert(id>ids[7]&&find(id)->identity.uid==11);
 assert(session_end_process(&backend,id)==MG_ERR_BUSY);dispatch();
 next_session_id=0;assert(session_create_process(&backend,1,"first",&full)==MG_ERR_BUSY);next_session_id=id+1;
 /* Exit wins before activation: no visible ACTIVE slot is published. */
 frontend.state=PROCESS_STATE_EXITING;
 assert(session_create_process(&backend,1,"first",&full)==MG_ERR_SHUTDOWN);
 frontend.state=PROCESS_STATE_ACTIVE;
 /* Activation wins: registry still excludes exit through publication. */
 ready();pause_stage=3;pthread_t t;u64 activated=0;assert(!pthread_create(&t,NULL,activate_host,&activated));wait_pause();
 assert(pthread_mutex_trylock(&process_registry_lock)==EBUSY);release_pause();pthread_join(t,NULL);
 frontend.state=PROCESS_STATE_TERMINATED;session_process_exited(&frontend);dispatch();assert(!find(activated));frontend.state=PROCESS_STATE_ACTIVE;
 /* Slot retirement/reuse after registry unlock cannot change the creator's
  * copied identity. This is the exact old escaped-slot counterexample. */
 ready();pause_stage=4;activated=0;
 assert(!pthread_create(&t,NULL,activate_host,&activated));wait_pause();
 u64 retired=0;for(unsigned i=0;i<SESSION_MAX_ACTIVE;i++)if(session_records[i].active)retired=session_records[i].id;
 assert(retired&&session_end_process(&backend,retired)==MG_ERR_BUSY);dispatch();
 u64 replacement=create("other");assert(replacement!=retired&&find(replacement)->identity.uid==22);
 release_pause();pthread_join(t,NULL);assert(activated==retired&&find(replacement)->identity.uid==22);
 assert(session_end_process(&backend,replacement)==MG_ERR_BUSY);dispatch();
 /* Simultaneous creators always receive distinct monotonic identities. */
 ready();pthread_t second;u64 created[2]={0};
 assert(!pthread_create(&t,NULL,activate_host,&created[0]));
 assert(!pthread_create(&second,NULL,activate_host,&created[1]));
 pthread_join(t,NULL);pthread_join(second,NULL);assert(created[0]&&created[1]&&created[0]!=created[1]);
 for(unsigned i=0;i<2;i++)assert(session_end_process(&backend,created[i])==MG_ERR_BUSY);
 dispatch();
 /* Exclusive PREPARING reservation rejects a second launch. Retirement wins
  * before permit, waits on creator, then observes complete rollback. */
 id=create("first");ready();pause_stage=1;struct call call={.id=id};assert(!pthread_create(&t,NULL,launch_host,&call));wait_pause();
 u32 handle=0;assert(session_launch_process(&frontend,id,&handle)==MG_ERR_BUSY);
 assert(session_end_process(&backend,id)==MG_ERR_BUSY);assert(dispatch()==1&&find(id));
 unsigned prior=retire_calls;for(unsigned i=0;i<10;i++)assert(session_end_process(&backend,id)==MG_ERR_BUSY);
 assert(!dispatch()&&retire_calls==prior);release_pause();pthread_join(t,NULL);assert(call.result==MG_ERR_SHUTDOWN);dispatch();assert(!find(id));
 /* Permit wins: even ENDING cannot steal the committed obligation. */
 id=create("first");ready();pause_stage=2;call=(struct call){.id=id};assert(!pthread_create(&t,NULL,launch_host,&call));wait_pause();
 assert(session_end_process(&backend,id)==MG_ERR_BUSY);assert(dispatch()==1&&find(id)->launch_phase==SESSION_LAUNCH_COMMITTED);
 release_pause();pthread_join(t,NULL);assert(call.result==MG_OK&&call.handle&&find(id)->shell_pid==call.handle);
 dispatch();assert(find(id)&&find(id)->state==MG_SESSION_STATE_ENDING&&cancel_wakes);
 assert(!dispatch());finish_member(id);dispatch();assert(!find(id));
 /* Failure before and after permit releases token; no slot gets stranded. */
 for(unsigned failure=1;failure<=2;failure++){id=create("first");ready();fail_stage=failure;
 assert(session_launch_process(&frontend,id,&handle)!=MG_OK&&find(id)->launch_phase==SESSION_LAUNCH_NONE);
 assert(session_end_process(&backend,id)==MG_ERR_BUSY);dispatch();assert(!find(id));}
 /* Early completed child is still the successfully registered exact shell. */
 id=create("first");ready();fail_stage=3;assert(session_launch_process(&frontend,id,&handle)==MG_OK);
 assert(find(id)->shell_pid==handle);finish_member(id);assert(session_end_process(&backend,id)==MG_ERR_BUSY);dispatch();assert(!find(id));
 /* Semantic progress remains after the originating member object vanishes. */
 id=create("first");ready();assert(session_launch_process(&frontend,id,&handle)==MG_OK);
 assert(session_end_process(&backend,id)==MG_ERR_BUSY);dispatch();finish_member(id);
 process_reap_session_members(id);assert(find(id)); /* No process carrier remains. */
 dispatch();assert(!find(id)&&destructions==children);
 /* Pre-ready publication is durable; generic notifications do not retire. */
 id=create("first");process_cleanup_worker_ready=false;unsigned before=wakes;
 assert(session_end_process(&backend,id)==MG_ERR_BUSY);assert(wakes==before&&process_cleanup_work_pending);
 process_cleanup_worker_ready=true;dispatch();assert(!find(id));
 id=create("other");session_process_exited(&backend);dispatch();assert(!find(id));
 assert(irq&&!lock_depth);puts("session lifecycle: identity/activation, exclusive token, both retirement/permit orders, rollback, cooperative exit, durable progress and exact child ownership PASS");
}
'''
with HostFixture('session-lifecycle') as fixture:
    source=fixture.path/'session.c';source.write_text(code)
    binary=fixture.path/'session'
    fixture.run([CC,'-std=gnu17','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
        '-fsanitize=address,undefined','-I'+str(ROOT/'kernel/include'),str(source),'-o',str(binary)])
    print(fixture.run([str(binary)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}).stdout,end='')
