#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production xHCI lifecycle/waits with simulated hardware and scheduling.
Faults are deterministic fixtures, not controller or QEMU integration tests.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'kernel/src/usb/xhci.c').read_text()

def function(text, name):
    for match in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(', text, re.M):
        brace = text.index('{', match.end())
        if ';' in text[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        return text[match.start():end] + '\n'
    raise ValueError(name)

def header(name):
    return '\n'.join(line for line in (ROOT / 'kernel/include/usb' / name).read_text().splitlines()
                     if not line.startswith('#include') and line != '#pragma once')

pre = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef unsigned long long u64; typedef size_t usize;
typedef struct {bool queued; int state;} kernel_thread_t;
typedef struct {void *virt; u64 phys;} dma_buffer_t;
typedef uintptr_t phys_addr_t;
#define XHCI_DEVICE_CLASS_HID 1U
typedef struct {unsigned unused;} xhci_storage_probe_result_t;
'''
headers = '\n'.join(header(n) for n in ('xhci_trb.h', 'xhci_context.h', 'xhci.h', 'xhci_regs.h', 'xhci_ring.h'))
structures = source[source.index('typedef struct {\n    bool pending;'):source.index('/* Accessors are implemented below')].replace('static bool g_xhc_init_started;', '')
model = r'''
static unsigned stop_calls, disable_calls, frees, ring_frees, removes, hid_removes, ticks;
static bool fail_stop, fail_disable, fail_halt, busy_storage, clock_fail;
static xhci_op_regs_t regs;
static xhci_ep_context_t contexts[32];
static xhci_trb_t ring_trbs[4];
static void kprint(const char *fmt, ...) {(void)fmt;}
void xhci_mark_event_work_pending(xhci_controller_t *x) {(void)x;}
bool xhci_start_deferred_worker(xhci_controller_t *x) {(void)x;return true;}
static void xhci_process_events(xhci_controller_t *x) {(void)x;}
void xhci_diag_set_phase(const char *p) {(void)p;}
static bool xhci_storage_prepare_teardown(xhci_controller_t *x,u8 slot)
{(void)x;(void)slot;return !busy_storage;}
static void xhci_storage_release_teardown_gate(xhci_controller_t*x,u8 s){(void)x;(void)s;}
static bool xhci_storage_remove_device(xhci_controller_t *x,u8 slot)
{assert(x->devices[slot].hardware_disabled);removes++;return true;}
static bool xhci_storage_device_teardown_pending(xhci_controller_t *x,u8 s)
{(void)x;(void)s;return false;}
void xhci_hid_remove_device(u8 s,u64 gen)
{(void)gen;assert(g_xhc_instance.devices[s].hardware_disabled);hid_removes++;}
static void dma_free(dma_buffer_t b) {(void)b;frees++;}
static uintptr_t virt_to_phys(void *v) {return (uintptr_t)v;}
static void pmm_free_frame(uintptr_t p) {(void)p;frees++;}
void xhci_dma_free(void *,usize);
void xhci_ring_free(xhci_ring_t *r) {if(r->trbs){ring_frees++;r->trbs=NULL;}}
xhci_status_t xhci_cmd_stop_endpoint(xhci_controller_t *x,u8 s,u8 d)
{assert(x->devices[s].quarantined);stop_calls++;if(fail_stop)return XHCI_ERR_TRANSACTION;
 ((xhci_ep_context_t*)x->devices[s].out_ctx_virt)[d].info1=XHCI_EP_STATE_STOPPED;
 return XHCI_SUCCESS;}
xhci_status_t xhci_cmd_disable_slot(xhci_controller_t *x,u8 s)
{assert(XHCI_EP_CTX_STATE_GET(((xhci_ep_context_t*)x->devices[s].out_ctx_virt)[1].info1)==XHCI_EP_STATE_STOPPED);
 disable_calls++;return fail_disable?XHCI_ERR_TRANSACTION:XHCI_SUCCESS;}
static bool xhci_wait_register_state(volatile u32 *p,u32 m,bool set)
{(void)p;(void)m;(void)set;return !fail_halt;}
static xhci_ring_t *xhci_get_ep_ring(xhci_controller_t *x,u8 s,u8 d)
{if(!xhci_slot_accepts_io(x,s))return NULL;return &x->devices[s].ep_rings[d];}
bool xhci_ring_trb_in_range(const xhci_ring_t*r,uintptr_t a,uintptr_t b,uintptr_t p)
{(void)r;return p>=a&&p<=b;}
typedef struct {unsigned ticks;} timer_monotonic_deadline_t;
static bool timer_monotonic_deadline_start(timer_monotonic_deadline_t*d,u64 us)
{(void)us;d->ticks=0;return !clock_fail;}
static bool timer_monotonic_deadline_expired(timer_monotonic_deadline_t*d)
{ticks++;return ++d->ticks>3;}
bool xhci_is_service_owner(xhci_controller_t*x){(void)x;return true;}
static bool scheduler_yield(void){return true;}
'''
names = ('xhci_accepts_enumeration','xhci_slot_accepts_io','xhci_quarantine_slot','xhci_quarantine_controller',
         'xhci_dma_may_release','xhci_dma_defer_release','xhci_arm_command_wait',
         'xhci_cancel_command_wait','xhci_take_command_completion','xhci_route_command_completion',
         'xhci_arm_transfer_operation','xhci_arm_transfer_wait','xhci_transfer_wait_generation',
         'xhci_disarm_unpublished_transfer_operation','xhci_disarm_unpublished_transfer_wait',
         'xhci_cancel_transfer_operation','xhci_cancel_transfer_wait','xhci_timeout_transfer_wait',
         'xhci_take_transfer_completion','xhci_route_transfer_event','xhci_stop','xhci_shutdown',
         'xhci_cleanup_failed_device')
production = ''.join(function(source,n) for n in names)
production += function((ROOT/'kernel/src/usb/xhci_mem.c').read_text(),'xhci_dma_free')
for name in ('xhci_map_completion_code','xhci_wait_for_cmd_completion','xhci_wait_for_transfer_completion'):
    production += function((ROOT/'kernel/src/usb/xhci_event.c').read_text(),name)
post = r'''
static xhci_controller_t *init(void){memset(&g_xhc_instance,0,sizeof(g_xhc_instance));
 memset(contexts,0,sizeof(contexts));memset(ring_trbs,0,sizeof(ring_trbs));
 xhci_controller_t*x=&g_xhc_instance;x->max_slots=2;x->op_regs=&regs;x->is_running=true;
 xhci_device_t*d=&x->devices[1];d->slot_id=1;d->instance_generation=42;
 d->state=XHCI_DEVICE_USB_CONFIGURED;d->out_ctx_virt=contexts;d->class_flags=XHCI_DEVICE_CLASS_HID;
 d->ep_rings[1]=(xhci_ring_t){.trbs=ring_trbs,.phys_base=0x1000,.size=4};
 contexts[1].info1=XHCI_EP_STATE_RUNNING;
 ring_trbs[0].param1=0x8000;ring_trbs[0].status=512;
 ring_trbs[0].control=XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_NORMAL);
 stop_calls=disable_calls=frees=ring_frees=removes=hid_removes=ticks=0;
 fail_stop=fail_disable=fail_halt=busy_storage=clock_fail=false;return x;}
static void arm(xhci_controller_t*x){assert(xhci_arm_transfer_wait(x,1,1,0x1000,0x1000,0x1000));}
int main(void){xhci_trb_t e={0};xhci_controller_t*x=init();arm(x);
 x->transfer_completion.status=XHCI_COMP_SUCCESS<<24;x->transfer_completion_ready=true;
 assert(xhci_wait_for_transfer_completion(x,&e)==XHCI_SUCCESS);
 assert(!x->devices[1].transfer_records[1][0].pending);assert(xhci_dma_may_release(0x8000,4096));
 x=init();arm(x);assert(xhci_wait_for_transfer_completion(x,&e)==XHCI_ERR_TIMEOUT);
 assert(x->devices[1].quarantined&&x->devices[1].transfer_records[1][0].pending);
 assert(xhci_slot_accepts_io(x,2)&&!xhci_accepts_enumeration(x));
 assert(!xhci_dma_may_release(0x8000,4096));assert(xhci_dma_may_release(0xa000,4096));
 xhci_dma_free((void*)0x8000,4096);assert(!frees);
 assert(x->devices[1].transfer_records[1][0].deferred_dma==(void*)0x8000);
 e=(xhci_trb_t){.param1=0x1000,.status=XHCI_COMP_SUCCESS<<24,.control=(1U<<24)|(1U<<16)};
 assert(xhci_route_transfer_event(x,&e)==XHCI_TRANSFER_EVENT_STALE);
 assert(!xhci_arm_transfer_wait(x,1,1,0x1000,0x1000,0x1000));
 busy_storage=true;assert(!xhci_cleanup_failed_device(x,&x->devices[1]));assert(!stop_calls&&!frees);
 busy_storage=false;assert(xhci_cleanup_failed_device(x,&x->devices[1]));
 assert(stop_calls==1&&disable_calls==1&&ring_frees==1&&frees==2&&hid_removes==1);
 assert(!xhci_cleanup_failed_device(x,&x->devices[1]));assert(stop_calls==1);
 x=init();arm(x);xhci_timeout_transfer_wait(x);fail_stop=true;
 assert(!xhci_cleanup_failed_device(x,&x->devices[1]));assert(!disable_calls&&!frees);
 assert(!xhci_cleanup_failed_device(x,&x->devices[1]));assert(stop_calls==1);
 x=init();arm(x);xhci_timeout_transfer_wait(x);fail_disable=true;
 assert(!xhci_cleanup_failed_device(x,&x->devices[1]));assert(!frees&&!ring_frees&&!hid_removes&&!removes);
 assert(!xhci_cleanup_failed_device(x,&x->devices[1]));assert(disable_calls==1);
 x=init();arm(x);fail_halt=true;xhci_shutdown(x);assert(x->quarantined&&!x->halt_confirmed);
 xhci_dma_free((void*)0x8000,4096);assert(!frees&&!ring_frees);
 assert(!xhci_arm_command_wait(x,XHCI_TRB_TYPE_DISABLE_SLOT,0x2000,1));
 xhci_shutdown(x);assert(!ring_frees);
 x=init();xhci_shutdown(x);assert(x->halt_confirmed&&!ring_frees);
 x=init();arm(x);x->transfer_completion.status=XHCI_COMP_STALL_ERR<<24;
 x->transfer_completion_ready=true;assert(xhci_wait_for_transfer_completion(x,&e)==XHCI_ERR_TRANSACTION);
 assert(x->devices[1].quarantined&&x->devices[1].transfer_records[1][0].pending);
 xhci_cancel_transfer_operation(x,1,1);assert(x->devices[1].transfer_records[1][0].pending);
 x=init();arm(x);xhci_cancel_transfer_wait(x);
 assert(x->devices[1].quarantined&&x->devices[1].transfer_records[1][0].pending);
 x=init();arm(x);xhci_disarm_unpublished_transfer_wait(x);
 assert(!x->devices[1].quarantined&&!x->devices[1].transfer_records[1][0].pending);
 x=init();arm(x);clock_fail=true;assert(xhci_wait_for_transfer_completion(x,&e)==XHCI_ERR_TIMEOUT);
 assert(x->devices[1].quarantined&&!ticks);
 x=init();assert(xhci_arm_command_wait(x,XHCI_TRB_TYPE_DISABLE_SLOT,0x2000,1));
 fail_halt=true;assert(xhci_wait_for_cmd_completion(x,XHCI_TRB_TYPE_DISABLE_SLOT,&e)==XHCI_ERR_TIMEOUT);
 assert(x->quarantined&&!x->halt_confirmed&&!x->command_waiting);
 e=(xhci_trb_t){.param1=0x2000,.status=XHCI_COMP_SUCCESS<<24,.control=1U<<24};
 assert(!xhci_route_command_completion(x,&e));
 x=init();assert(xhci_arm_command_wait(x,XHCI_TRB_TYPE_DISABLE_SLOT,0x2000,1));
 x->command_completion.status=XHCI_COMP_SHORT_PACKET<<24;x->command_completion_ready=true;
 assert(xhci_wait_for_cmd_completion(x,XHCI_TRB_TYPE_DISABLE_SLOT,&e)==XHCI_ERR_CONTROLLER_BAD);
 puts("PASS: xHCI completion, timeout, late completion, failed stop/disable/halt, DMA retention, admission, repeated teardown, missing clock");}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-xhci-') as work:
    path=Path(work)/'test.c';path.write_text(pre+headers+structures+model+production+post)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc','-std=c11','-Wall','-Wextra',
                    '-Wno-address-of-packed-member','-fsanitize=address,undefined',str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
