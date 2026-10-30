#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production IPC waiter state and scheduler event publication interleavings."""
from pathlib import Path
import os
import re
import resource
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/shared'))
from host_fixture import HostFixture
CC = sys.argv[1] if len(sys.argv) > 1 else 'gcc'


def function(source, name):
    for match in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(', source, re.M):
        brace = source.index('{', match.end())
        if ';' in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end]
    raise ValueError(name)


ipc = (ROOT / 'kernel/src/ipc.c').read_text()
ipc = ipc.replace(function(ipc, 'ipc_cli_save'),
    'static u64 ipc_cli_save(void){u64 f=host_if;host_if=false;return f;}')
ipc = ipc.replace(function(ipc, 'ipc_cli_restore'),
    'static void ipc_cli_restore(u64 f){host_if=f!=0;}')
code = '#include <stdbool.h>\nstatic bool host_if=true;\n' + ipc
code += r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define THREAD_CPU_NONE (~(u32)0)
static unsigned depth;
static process_t client, service;
static kernel_thread_t receiver, foreign, *current_thread=&receiver;
static kernel_thread_t *idle_thread, *sleeping_threads;
static spinlock_t scheduler_lock;
static scheduler_stats_t scheduler_stats;
static bool preemption_pending;
static ipc_endpoint_t endpoint;
static ipc_request_object_t reply_context;
static u64 now;
static int mode;
static bool enqueue_failure;
static u32 cpu_current_index(void){return 0;}
static bool scheduler_enqueue_locked(kernel_thread_t*t){assert(!t->queued);if(enqueue_failure)return false;t->queued=true;return true;}
void *kmalloc(usize n){return malloc(n);}
void kfree(void*p){free(p);}
void panic(const char*s){fprintf(stderr,"%s\n",s);abort();}
void spinlock_init(spinlock_t*l){l->value=0;}
u64 spin_lock_irqsave(spinlock_t*l){u64 f=host_if;host_if=false;assert(!l->value);l->value=1;depth++;return f;}
void spin_unlock_irqrestore(spinlock_t*l,u64 f){assert(l->value&&depth);l->value=0;depth--;host_if=f!=0;}
void spin_lock(spinlock_t*l){assert(!host_if&&!l->value);l->value=1;depth++;}
void spin_unlock(spinlock_t*l){assert(l->value&&depth);l->value=0;depth--;}
void object_init(kernel_object_t*o,kernel_object_type_t t,void(*destroy)(kernel_object_t*)){memset(o,0,sizeof(*o));o->type=t;o->ref_count=1;o->destroy=destroy;}
void object_release(kernel_object_t*o){assert(o->ref_count);if(!--o->ref_count&&o->destroy)o->destroy(o);}
bool object_reference(kernel_object_t*o){assert(o->ref_count);o->ref_count++;return true;}
process_t*process_current(void){return &client;}
kernel_thread_t*thread_current(void){return &receiver;}
kernel_object_t*process_handle_lookup(process_t*p,process_handle_t h,kernel_object_type_t t,u32 rights){(void)p;(void)t;(void)rights;kernel_object_t*o=h==2?&reply_context.object:&endpoint.object;object_reference(o);return o;}
bool process_handle_install(process_t*p,kernel_object_t*o,u32 rights,process_handle_t*h){(void)p;(void)rights;object_reference(o);*h=3;return true;}
bool process_get_credentials(const process_t*p,process_credentials_t*c){(void)p;memset(c,0,sizeof(*c));return true;}
bool process_shutdown_requested(const process_t*p){return p->shutdown_requested;}
bool process_take_interrupt(process_t*p){if(mode==8&&!p->shutdown_requested){p->shutdown_requested=true;assert(scheduler_wake_waiter_result(&receiver)==SCHEDULER_WAKE_DELIVERED);}bool value=p->interrupt_requested;p->interrupt_requested=false;return value;}
u64 timer_uptime_ms(void){return now;}
'''
scheduler = (ROOT / 'kernel/src/scheduler.c').read_text()
for name in ('sleeping_remove', 'scheduler_unblock_locked',
             'scheduler_wake_waiter_result', 'scheduler_clear_event_wake',
             'scheduler_consume_pending_wake_locked'):
    code += '\n' + function(scheduler, name)
code += r'''
static bool consume(void){u64 f=spin_lock_irqsave(&scheduler_lock);bool result=scheduler_consume_pending_wake_locked(&receiver,true);spin_unlock_irqrestore(&scheduler_lock,f);return result;}
bool scheduler_sleep(u64 ms){assert(!depth&&ms);assert(endpoint.receiver_waiter==&receiver);
 if(mode==1){receiver.state=THREAD_STATE_RUNNING;ipc_publish_event(1,1,1,NULL);assert(consume());}
 else if(mode==2){now+=ms;}
 else if(mode==3){u64 f=ipc_irq_save();endpoint.event_overflow=true;endpoint_wake_receiver_locked(&endpoint);ipc_irq_restore(f);assert(consume());}
 else if(mode==4){endpoint_unregister(&endpoint);}
 else if(mode==5){client.interrupt_requested=true;}
 else if(mode==6){client.shutdown_requested=true;}
 else if(mode==7){return false;}
 else if(mode==8){assert(consume());}
 return true;}
bool scheduler_block(void){assert(!depth&&client.ipc_outstanding);ipc_request_t*q=client.ipc_outstanding;
 assert(q->client_blocked&&q->client_thread==&receiver);ipc_delivery_t delivery={0};
 u64 f=ipc_irq_save();assert(endpoint_dequeue(&endpoint,&delivery));assert(delivery.request==q);q->state=IPC_REQUEST_DELIVERED;q->delivery_active=false;q->context_live=true;
 object_init(&reply_context.object,OBJECT_TYPE_IPC_REQUEST,NULL);reply_context.owner=&service;reply_context.request=q;ipc_irq_restore(f);
 mg_ipc_message_t reply={.version=MG_IPC_PROTOCOL_VERSION,.type=1};assert(ipc_kernel_reply(&service,2,&reply)==MG_OK);assert(consume());return true;}
static void reset(void){memset(&receiver,0,sizeof(receiver));memset(&client,0,sizeof(client));memset(&service,0,sizeof(service));memset(&endpoint,0,sizeof(endpoint));object_init(&endpoint.object,OBJECT_TYPE_IPC_ENDPOINT,NULL);endpoint.owner=&client;endpoint.live=true;endpoint.event_classes=1;endpoint_list=&endpoint;receiver.state=THREAD_STATE_RUNNING;now=0;assert(!depth&&host_if);}
int main(int argc,char**argv){mg_ipc_received_t received;
 if(argc>1){(void)argv;reset();receiver.state=THREAD_STATE_BLOCKED;enqueue_failure=true;u64 f=ipc_irq_save();ipc_request_t q={.client_active=true,.client_blocked=true,.client_thread=&receiver};request_wake_client(&q);ipc_irq_restore(f);return 0;}
 reset();u64 f=ipc_irq_save();ipc_request_t q={.client_active=true,.client_blocked=true,.client_thread=&receiver};request_wake_client(&q);assert(!q.client_thread&&!q.client_blocked);ipc_irq_restore(f);assert(consume());
 reset();receiver.state=THREAD_STATE_BLOCKED;f=ipc_irq_save();q=(ipc_request_t){.client_active=true,.client_blocked=true,.client_thread=&receiver};request_wake_client(&q);ipc_irq_restore(f);assert(receiver.state==THREAD_STATE_READY&&receiver.queued);
 for(mode=1;mode<=8;mode++){reset();int result=ipc_kernel_receive_timed(&client,1,&received,10);assert(!endpoint.receiver_waiter&&!receiver.event_wake_pending&&!depth&&host_if);
 assert(result==(mode==2?MG_ERR_TIMEOUT:mode==4?MG_ERR_SERVICE_UNAVAILABLE:mode==5?MG_ERR_INTERRUPTED:mode==6?MG_ERR_SHUTDOWN:mode==7?MG_ERR_BUSY:mode==8?MG_ERR_SHUTDOWN:MG_OK));}
 reset();endpoint.receiver_waiter=&foreign;assert(ipc_kernel_receive_timed(&client,1,&received,10)==MG_ERR_BUSY);assert(endpoint.receiver_waiter==&foreign);endpoint.receiver_waiter=NULL;
 reset();mg_ipc_message_t message={.version=MG_IPC_PROTOCOL_VERSION,.type=1},reply;assert(ipc_kernel_request(&client,1,&message,&reply)==MG_OK);assert(!client.ipc_outstanding&&!receiver.event_wake_pending);f=ipc_irq_save();reply_context.request->context_live=false;request_maybe_free(reply_context.request);ipc_irq_restore(f);request_reclaim_retired();
 assert(!depth&&host_if);puts("IPC production wait loops: running/blocked wake, reply-before-block, timed receive, overflow, cancellation, endpoint loss, competing receiver and registration cleanup PASS");return 0;}
'''
with HostFixture('ipc-waiter') as fixture:
    source = fixture.path / 'ipc_waiter.c'
    binary = fixture.path / 'ipc_waiter'
    source.write_text(code)
    fixture.run([CC, '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-function', '-Wno-unused-variable', '-fno-builtin',
                 '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
                 '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'include'),
                 '-I'+str(ROOT/'libc/include'), str(source), '-Wl,--gc-sections',
                 '-o', str(binary)])
    print(fixture.run([str(binary)], env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'}).stdout, end='')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    failure = fixture.run([str(binary), "live-wake-failure"], check=False,
        env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'})
    assert failure.returncode != 0 and 'live registered waiter' in failure.stderr
