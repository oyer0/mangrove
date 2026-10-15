#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production FIFO/block/xHCI admission, controlled hardware and scheduler.
The hardware setup callback is a boundary fixture; real enumeration is guest tested.
"""
import ast
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]

def function(source,name):
    match=re.search(r'^(?:static )?[A-Za-z_][^\n;{}]*\b'+name+r'\s*\(',source,re.M)
    start=source.index('{',match.end());end=start+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]+'\n'

def stripped(source):
    return re.sub(r'^(?:#include .*|#pragma once)\n','',source,flags=re.M)

module=ast.parse((ROOT/'tests/mutex_test.py').read_text())
pre=next(ast.literal_eval(n.value) for n in module.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='pre' for t in n.targets))
mutex=stripped((ROOT/'kernel/src/mutex.c').read_text())
mutex=re.sub(r'static u64 mutex_irq_save\(void\).*?\n}', 'static u64 mutex_irq_save(void){return 0;}',mutex,count=1,flags=re.S)
mutex=re.sub(r'static void mutex_irq_restore\(u64 flags\).*?\n}', 'static void mutex_irq_restore(u64 flags){(void)flags;}',mutex,count=1,flags=re.S)
wake=function((ROOT/'kernel/src/scheduler.c').read_text(),'scheduler_mutex_wake')
block=(ROOT/'kernel/src/block.c').read_text()
gate=block[block.index('static mutex_t block_io_gate;'):block.index('static bool block_cache_range_overlaps')]
xhci=(ROOT/'kernel/src/usb/xhci.c').read_text()
structures=xhci[xhci.index('typedef struct {\n    bool pending;'):xhci.index('/* Accessors are implemented below')].replace('static bool g_xhc_init_started;','')
headers=''.join(stripped((ROOT/'kernel/include/usb'/n).read_text()) for n in ('xhci_trb.h','xhci_context.h','xhci.h','xhci_regs.h','xhci_ring.h'))
management=(ROOT/'kernel/src/storage/format.c').read_text()
management_functions=('static bool storage_operation_in_progress;\n'+function(management,'storage_mutation_begin')+
    # Observe ordering at the release dependency, then invoke the real gate.
    'static void management_release_observer(void){assert(!storage_operation_in_progress);block_io_end_quiesce();}\n'+
    '#define block_io_end_quiesce management_release_observer\n'+function(management,'storage_mutation_end')+
    '#undef block_io_end_quiesce\n')
fixture=r'''
static volatile u32 portsc[256];
static unsigned setups, removals, wakes, failure;
static bool offline_before_grant;
static kernel_thread_t owner, service, competitor;
static xhci_trb_t trbs[4];
static void kprint(const char *fmt,...){(void)fmt;}
static u64 timer_uptime_ms(void){return 0;}
static void kprint_debug(const char *fmt,...){(void)fmt;}
bool xhci_is_service_owner(xhci_controller_t*x){return self==x->deferred_worker;}
void xhci_mark_event_work_pending(xhci_controller_t*x){x->event_work_pending=true;wakes++;}
bool xhci_start_deferred_worker(xhci_controller_t*x){(void)x;return true;}
volatile u32*xhci_get_portsc_ptr(xhci_controller_t*x,u8 p){return p&&p<=x->max_ports?&portsc[p]:NULL;}
void xhci_set_port_state(xhci_controller_t*x,u8 p,xhci_port_state_t s,u32 v,const char*r){(void)v;(void)r;x->ports[p].state=s;}
bool xhci_detach_device(xhci_controller_t*x,u8 port){
 (void)x;(void)port;
 if(block_io_gate.owner!=self || !block_io_depth)offline_before_grant=true;
 return false; /* Transport teardown remains pending at this controlled boundary. */
}
static void xhci_process_pending_teardowns(xhci_controller_t*x){(void)x;}
static void xhci_process_deferred_port_change(xhci_controller_t*x,u8 p){
 assert(block_io_gate.owner==self&&block_io_depth==1&&!block_io_callbacks);
 assert(x->enumeration_reserved&&!x->transfer_waiting);
 if(!(portsc[p]&XHCI_PORTSC_CCS)){removals++;return;}
 setups++;
 /* Actual recursive admission used by GPT/registration must succeed. */
 assert(storage_mutation_begin());storage_mutation_end();
 if(failure==1)x->ports[p].state=XHCI_PORT_FAILED;
 if(failure==2)x->quarantined=true;
 if(failure==3)x->deferred_worker_stop=true;
}
void xhci_quarantine_slot(xhci_controller_t*x,u8 s){x->devices[s].quarantined=true;}
void xhci_quarantine_controller(xhci_controller_t*x){x->quarantined=true;}
'''
names=('xhci_slot_accepts_io','xhci_queue_port_change','xhci_take_port_change',
       'xhci_cancel_port_admission','xhci_process_disconnects','xhci_process_port_work','xhci_port_work_ready',
       'xhci_sync_waiter_released','xhci_arm_transfer_operation','xhci_arm_transfer_wait',
       'xhci_take_transfer_completion','xhci_cancel_transfer_wait','xhci_device_attachment_current',
       'xhci_boot_enumeration_quiescent','xhci_shutdown')
post=r'''
static xhci_controller_t *init(void){
 memset(&g_xhc_instance,0,sizeof(g_xhc_instance));memset((void*)portsc,0,sizeof(portsc));
 xhci_controller_t*x=&g_xhc_instance;x->max_ports=3;x->max_slots=3;x->deferred_worker=&service;
 x->state=XHCI_CONTROLLER_SERVICE_READY;x->devices[1].slot_id=1;
 x->devices[1].instance_generation=1;x->devices[1].ep_rings[3]=(xhci_ring_t){.trbs=trbs,.phys_base=0x1000,.size=4};
 x->devices[2].slot_id=2;x->devices[2].instance_generation=2;
 x->devices[2].ep_rings[1]=(xhci_ring_t){.trbs=trbs,.phys_base=0x2000,.size=4};
 portsc[3]=XHCI_PORTSC_CCS;failure=0;self=&owner;return x;
}
static void idle(void){assert(!block_io_depth&&!block_io_callbacks&&!block_io_gate.owner&&!block_io_gate.wait_head);}
int main(void){
 owner.state=service.state=competitor.state=THREAD_STATE_RUNNING;
 pthread_cond_init(&owner.condition,NULL);pthread_cond_init(&service.condition,NULL);pthread_cond_init(&competitor.condition,NULL);
 xhci_controller_t*x=init();
 /* Historical conflict: ordinary BOT transfer owns the single waiter. */
 assert(block_io_acquire());block_callback_enter();
 assert(xhci_arm_transfer_wait(x,1,3,0x1000,0x1000,0x1000));
 self=&service;assert(!xhci_arm_transfer_wait(x,2,1,0x2000,0x2000,0x2000));
 xhci_queue_port_change(x,3,"test");xhci_process_port_work(x);
 assert(x->port_admission_state==1&&!setups&&!xhci_port_work_ready(x));
 assert(!xhci_boot_enumeration_quiescent(x));
 self=&competitor;assert(block_io_defer_quiesce()==0);
 self=&owner;x->transfer_completion.status=XHCI_COMP_SUCCESS<<24;x->transfer_completion_ready=true;
 xhci_trb_t e;assert(xhci_take_transfer_completion(x,&e));
 self=&service;assert(!xhci_port_work_ready(x)); /* Waiter release is too early. */
 self=&owner;assert(block_io_acquire());block_callback_leave();block_io_release();
 self=&service;assert(!xhci_port_work_ready(x));
 self=&owner;block_io_release();self=&service;
 assert(xhci_port_work_ready(x)&&service.mutex_wake_pending);
 assert(!block_io_begin_quiesce()); /* Unconsumed grant cannot be stolen. */
 xhci_process_port_work(x);assert(setups==1&&!x->port_admission_state);
 self=&competitor;assert(block_io_defer_quiesce()==1);block_io_cancel_deferred_quiesce();
 assert(block_io_depth==1);block_io_end_quiesce();idle();
 /* Owner completes before the request; immediate acquisition still works. */
 x=init();self=&service;xhci_queue_port_change(x,3,"test");xhci_process_port_work(x);assert(setups==2);idle();
 /* A management release clears its busy flag before granting enumeration. */
 x=init();assert(storage_mutation_begin());self=&service;
 xhci_queue_port_change(x,3,"management");xhci_process_port_work(x);
 self=&owner;storage_mutation_end();self=&service;xhci_process_port_work(x);idle();
 /* Cancellation before and after handoff. */
 for(unsigned granted=0;granted<2;granted++){
  x=init();assert(block_io_acquire());self=&service;xhci_queue_port_change(x,3,"test");xhci_process_port_work(x);
  if(granted){self=&owner;block_io_release();self=&service;}
  xhci_cancel_port_admission(x);assert(!service.waiting_mutex);
  if(!granted){self=&owner;block_io_release();}idle();
 }
 /* Superseding generation cannot enumerate the old request. */
 x=init();assert(block_io_acquire());self=&service;xhci_queue_port_change(x,3,"test");xhci_process_port_work(x);
 unsigned before=setups;portsc[3]=0;xhci_queue_port_change(x,3,"disconnect");xhci_process_port_work(x);
 self=&owner;block_io_release();self=&service;xhci_process_port_work(x);
 assert(setups==before&&removals==1&&offline_before_grant);idle();
 /* Controller stop/quarantine cancels a queued or unconsumed request. */
 for(unsigned granted=0;granted<2;granted++){
  x=init();assert(block_io_acquire());self=&service;
  xhci_queue_port_change(x,3,"test");xhci_process_port_work(x);
  if(granted){self=&owner;block_io_release();self=&service;}
  x->quarantined=true;xhci_process_port_work(x);
  assert(!x->port_admission_state&&!xhci_port_work_ready(x)&&!service.waiting_mutex);
  if(!granted){self=&owner;block_io_release();}idle();
 }
 /* Shutdown cannot lose its cancellation wake just before the worker parks. */
 x=init();assert(block_io_acquire());self=&service;
 xhci_queue_port_change(x,3,"shutdown");xhci_process_port_work(x);
 self=&owner;xhci_shutdown(x);self=&service;
 assert(service.mutex_wake_pending&&scheduler_block());
 xhci_process_port_work(x);assert(!service.waiting_mutex);
 self=&owner;block_io_release();idle();
 /* Coalesced repeated events preserve one request and enumerate once. */
 x=init();assert(block_io_acquire());self=&service;before=setups;
 for(unsigned n=0;n<16;n++){
  xhci_queue_port_change(x,3,"repeat");xhci_process_port_work(x);
  assert(block_io_gate.wait_head==&service&&block_io_gate.wait_tail==&service);
 }
 self=&owner;block_io_release();self=&service;xhci_process_port_work(x);
 assert(setups==before+1);idle();
 /* Slot mapping publication rejects stale root-port generations. */
 x=init();x->devices[2].port_id=3;x->devices[2].attach_port_generation=0;
 assert(xhci_device_attachment_current(x,2));xhci_queue_port_change(x,3,"replacement");
 assert(!xhci_device_attachment_current(x,2));
 /* External non-block waiter releases after deferral, or cancels. */
 for(unsigned cancel=0;cancel<2;cancel++){
  x=init();assert(xhci_arm_transfer_wait(x,1,3,0x1000,0x1000,0x1000));self=&service;
  xhci_queue_port_change(x,3,"test");xhci_process_port_work(x);
  assert(x->port_admission_state==2&&!xhci_port_work_ready(x));idle();self=&owner;
  if(cancel)xhci_cancel_transfer_wait(x);else{x->transfer_completion_ready=true;x->transfer_completion.status=XHCI_COMP_SUCCESS<<24;assert(xhci_take_transfer_completion(x,&e));}
  self=&service;assert(xhci_port_work_ready(x)&&service.mutex_wake_pending);
  assert(scheduler_block());assert(!service.mutex_wake_pending);
  xhci_process_port_work(x);idle();
 }
 /* Failed setup, controller timeout/quarantine, shutdown: release the grant. */
 for(unsigned f=1;f<=3;f++){
  x=init();self=&service;failure=f;xhci_queue_port_change(x,3,"test");xhci_process_port_work(x);idle();
  assert(!x->port_admission_state&&!x->enumeration_reserved);
 }
 /* A conflicting request is an admission error, never a fake acquisition. */
 x=init();self=&service;mutex_t other;mutex_init(&other);service.waiting_mutex=&other;
 xhci_queue_port_change(x,3,"test");xhci_process_port_work(x);assert(x->ports[3].state==XHCI_PORT_FAILED);
 service.waiting_mutex=NULL;idle();
 for(unsigned n=0;n<10000;n++){
  x=init();assert(block_io_acquire());block_callback_enter();self=&service;
  xhci_queue_port_change(x,3,"test");xhci_process_port_work(x);xhci_process_port_work(x);
  assert(block_io_gate.wait_head==&service&&block_io_gate.wait_tail==&service);
  self=&owner;block_callback_leave();block_io_release();self=&service;xhci_process_port_work(x);idle();
 }
 printf("PASS: production xHCI/FIFO/block deferred admission; waiter-before-gate, FIFO/no overtaking, nested release, cancellation, generations, failure cleanup, 10000 repeated handoffs; host-fixture mutex=%zu bytes (pthread lock), controller=%zu\n",sizeof(mutex_t),sizeof(*x));
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-xhci-admission-') as tmp:
    path=Path(tmp)/'test.c'
    types='\n#include <string.h>\ntypedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef size_t usize;typedef uintptr_t phys_addr_t;typedef struct{void*virt;u64 phys;}dma_buffer_t;\n#define XHCI_DEVICE_CLASS_HID 1U\ntypedef struct{unsigned unused;}xhci_storage_probe_result_t;\n'
    path.write_text(pre+types+wake+mutex+gate+'\n#undef __atomic_store_n\n'+headers+structures+management_functions+fixture+''.join(function(xhci,n) for n in names)+post)
    exe=path.with_suffix('')
    subprocess.run([sys.argv[1]if len(sys.argv)>1 else 'cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Werror','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},check=True,timeout=30)
