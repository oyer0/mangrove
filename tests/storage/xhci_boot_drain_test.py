#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The bootstrap gate must obtain an event-owner pass even without an IRQ."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/src/usb/xhci.c').read_text()
def function(name):
    m=re.search(r'^(?:static )?(?:bool|void|xhci_status_t) '+name+r'\(',source,re.M)
    start=source.index('{',m.end());end=start+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[m.start():end]+'\n'
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;typedef unsigned xhci_port_state_t;
typedef struct {u32 unused;} xhci_op_regs_t;
typedef int xhci_status_t;
#define XHCI_SUCCESS 0
#define XHCI_ERR_TIMEOUT 2
#define XHCI_ERR_INVALID_PARAM 3
#define XHCI_ERR_CONTROLLER_BAD 6
#define XHCI_PORT_CONNECTED_OBSERVED 4
#define XHCI_PORTSC_CCS 1U
#define XHCI_BOOT_QUIESCENCE_TIMEOUT_MS 10U
#define XHCI_PORT_DISCONNECTED 0
#define XHCI_PORT_READY 1
#define XHCI_PORT_FAILED 2
#define XHCI_PORT_ENUMERATING 3
typedef struct {
 bool quarantined,deferred_worker_stop;
 bool in_critical_section,enumeration_reserved,command_waiting,transfer_waiting,event_work_pending;
 u8 admission_port;
 u32 port_admission_state,pending_port_changes[8],max_ports;
 u32 boot_event_drain_requested,boot_event_drain_completed;
 struct {unsigned state;} ports[3];
 xhci_op_regs_t *op_regs;
} xhci_controller_t;
static xhci_op_regs_t *xhci_get_op_regs(xhci_controller_t *x){return x->op_regs;}
static u8 xhci_get_max_ports(xhci_controller_t *x){return x->max_ports;}
static unsigned queued,setups;
static bool reconcile,wrong_root,service_owner,retained_slot,sleep_fails;
static bool xhci_is_service_owner(xhci_controller_t*x){(void)x;return service_owner;}
static u8 xhci_device_slot_for_port(xhci_controller_t*x,u8 p){(void)x;(void)p;return retained_slot?1:0;}
static u64 attach_at,finish_at,publish_at;
static _Alignas(16) u8 fake_mmio[8192];
static void xhci_set_port_state(xhci_controller_t*x,u8 p,xhci_port_state_t state,u32 status,const char*r){
 (void)status;(void)r;x->ports[p].state=state;
}
static volatile u32 *xhci_get_portsc_ptr(xhci_controller_t*,u8);
static void xhci_reconcile_boot_ports(xhci_controller_t*);
static void xhci_queue_port_change(xhci_controller_t*x,u8 p,const char*r){
 (void)r;assert(!(x->pending_port_changes[p/32]&(1U<<(p%32))));
 x->pending_port_changes[p/32]|=1U<<(p%32);queued++;
}
static u64 now;
static unsigned starts,passes,sleeps;
static bool worker_available,withhold_completion,attach_on_pass,hold_setup,stale_pass_first;
static xhci_controller_t *controller;
static u64 timer_uptime_ms(void){return now;}
static bool xhci_start_deferred_worker(xhci_controller_t *x){assert(x==controller);starts++;return worker_available;}
static void xhci_mark_event_work_pending(xhci_controller_t *x){x->event_work_pending=true;}
static bool scheduler_sleep(u64 ms){
 if(sleep_fails)return false;
 assert(ms==1);now++;sleeps++;
 if(!withhold_completion){
  passes++;controller->event_work_pending=false;
  if(reconcile){
   if(now>=attach_at)*xhci_get_portsc_ptr(controller,1)=0x6e1; /* USB2 Polling, CSC already clear */
   xhci_reconcile_boot_ports(controller);
   if(controller->pending_port_changes[0]&2){
    controller->pending_port_changes[0]&=~2U;setups++;
    controller->ports[1].state=XHCI_PORT_ENUMERATING;
   }
   if(setups&&now>=finish_at)controller->ports[1].state=XHCI_PORT_READY;
  }
  controller->boot_event_drain_completed=controller->boot_event_drain_requested - (stale_pass_first&&sleeps==1?1U:0U);
  /* The sole consumer dispatches a queued attach before acknowledging its pass. */
  if(attach_on_pass)controller->ports[1].state=XHCI_PORT_READY;
  if(hold_setup)controller->ports[1].state=sleeps<3?XHCI_PORT_ENUMERATING:XHCI_PORT_READY;
 }
 return true;
}
'''
ps=(ROOT/'kernel/src/usb/xhci_port.c').read_text()
a=ps.index('volatile u32* xhci_get_portsc_ptr(');b=ps.index('{',a)+1;depth=1
while depth:
    depth+=(ps[b]=='{')-(ps[b]=='}');b+=1
port_accessor=ps[a:b]+'\n'
suffix=r'''
static void reset(xhci_controller_t *x){
 *x=(xhci_controller_t){.max_ports=2,.op_regs=(xhci_op_regs_t*)fake_mmio};controller=x;
 for(unsigned i=0;i<sizeof(fake_mmio);i++)fake_mmio[i]=0;
 queued=setups=0;reconcile=wrong_root=service_owner=retained_slot=sleep_fails=false;attach_at=3;finish_at=5;publish_at=6;
 now=starts=passes=sleeps=0;worker_available=true;
 withhold_completion=attach_on_pass=hold_setup=stale_pass_first=false;
}
static bool root_ready(void *context){
 assert(context==controller);return !wrong_root&&setups&&now>=publish_at;
}
static bool existing_root(void *context){assert(context==controller);return true;}
int main(void){
 xhci_controller_t x;reset(&x);
 _Alignas(16) static u8 mmio[8192];
 x.max_ports=15;
 for(unsigned caplen=0x20;caplen<=0x80;caplen+=0x60){
  x.op_regs=(xhci_op_regs_t*)(mmio+caplen);
  assert((uintptr_t)xhci_get_portsc_ptr(&x,1)==(uintptr_t)mmio+caplen+0x400);
  assert((uintptr_t)xhci_get_portsc_ptr(&x,15)==(uintptr_t)mmio+caplen+0x4e0);
  assert(xhci_get_portsc_ptr(&x,0)==(void*)0&&xhci_get_portsc_ptr(&x,16)==(void*)0);
 }
 reset(&x);attach_on_pass=true;
 /* Initial software flags and CCS scan are empty, but an event is queued in
    hardware while MSI-X is masked. The old gate returned without a drain. */
 assert(xhci_wait_for_boot_quiescence(&x));
 assert(passes>0&&starts>0&&x.ports[1].state==XHCI_PORT_READY);
 reset(&x);assert(xhci_wait_for_boot_quiescence(&x)&&passes==1);
 assert(xhci_wait_for_boot_quiescence(&x)&&passes==2); /* fresh pass */
 reset(&x);stale_pass_first=true;
 assert(xhci_wait_for_boot_quiescence(&x)&&passes==2);
 reset(&x);withhold_completion=true;
 assert(!xhci_wait_for_boot_quiescence(&x)&&now==10);
 reset(&x);worker_available=false;
 assert(!xhci_wait_for_boot_quiescence(&x)&&passes==0);
 reset(&x);hold_setup=true;
 assert(xhci_wait_for_boot_quiescence(&x)&&sleeps==3);
 assert(!xhci_wait_for_boot_quiescence((void*)0));
 /* CCS becomes visible after the first idle pass, without a surviving CSC. */
 reset(&x);reconcile=true;
 assert(xhci_wait_for_boot_quiescence(&x)&&now==1&&setups==0);
 assert(xhci_wait_for_boot_discovery(&x,root_ready,&x)==XHCI_SUCCESS);
 assert(now>=publish_at&&now<10&&queued==1&&setups==1&&x.ports[1].state==XHCI_PORT_READY);
 /* An enumerated disk without the required GPT identity is not readiness. */
 reset(&x);reconcile=true;wrong_root=true;
 assert(xhci_wait_for_boot_discovery(&x,root_ready,&x)==XHCI_ERR_TIMEOUT);
 assert(now==10&&setups==1&&queued==1);
 /* No device also gets a bounded opportunity; an existing root incurs only
    the fresh owner fence, never the entire publication deadline. */
 reset(&x);
 assert(xhci_wait_for_boot_discovery(&x,root_ready,&x)==XHCI_ERR_TIMEOUT);
 assert(now==10&&!queued&&!setups&&x.ports[1].state==XHCI_PORT_DISCONNECTED);
 reset(&x);
 assert(xhci_wait_for_boot_discovery(&x,existing_root,&x)==XHCI_SUCCESS);
 assert(now==1&&passes==1&&!queued&&!setups);
 /* Terminal failures and live setup must never be reset/retried by CCS reconciliation. */
 reset(&x);reconcile=true;x.ports[1].state=XHCI_PORT_FAILED;
 assert(xhci_wait_for_boot_discovery(&x,root_ready,&x)==XHCI_ERR_TIMEOUT);
 assert(!setups&&!queued);
 reset(&x);*xhci_get_portsc_ptr(&x,1)=0x6e1;
 x.ports[1].state=XHCI_PORT_READY;xhci_reconcile_boot_ports(&x);assert(!queued);
 x.ports[1].state=XHCI_PORT_CONNECTED_OBSERVED;x.pending_port_changes[0]=2;
 xhci_reconcile_boot_ports(&x);assert(!queued);
 x.pending_port_changes[0]=0;x.port_admission_state=1;x.admission_port=1;
 xhci_reconcile_boot_ports(&x);assert(!queued);
 x.port_admission_state=0;retained_slot=true;
 xhci_reconcile_boot_ports(&x);assert(!queued);
 reset(&x);service_owner=true;
 assert(xhci_wait_for_boot_discovery(&x,root_ready,&x)==XHCI_ERR_CONTROLLER_BAD);
 assert(!starts&&!sleeps); /* The completion owner must never wait for itself. */
 reset(&x);sleep_fails=true;
 assert(xhci_wait_for_boot_discovery(&x,root_ready,&x)==XHCI_ERR_CONTROLLER_BAD);
 assert(now==0);
 reset(&x);x.quarantined=true;
 assert(xhci_wait_for_boot_discovery(&x,root_ready,&x)==XHCI_ERR_CONTROLLER_BAD);
 reset(&x);worker_available=false;
 assert(xhci_wait_for_boot_discovery(&x,root_ready,&x)==XHCI_ERR_CONTROLLER_BAD);
 assert(xhci_wait_for_boot_discovery((void*)0,root_ready,&x)==XHCI_ERR_INVALID_PARAM);
 assert(xhci_wait_for_boot_discovery(&x,(void*)0,&x)==XHCI_ERR_INVALID_PARAM);

}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-xhci-boot-drain-') as tmp:
    c=Path(tmp)/'test.c';binary=Path(tmp)/'test'
    c.write_text(prefix+port_accessor+function('xhci_reconcile_boot_ports')+function('xhci_boot_enumeration_quiescent')+function('xhci_boot_drain_until')+function('xhci_wait_for_boot_quiescence')+function('xhci_wait_for_boot_discovery')+suffix)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
print('PASS: late CCS without CSC, required-root publication, stale acknowledgements, wrong identity, no failed-port retries, timeout and worker failure')
