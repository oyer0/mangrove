#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production xHCI class-match and low-level failure policy regressions."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    pattern = re.compile(r'^(?:static\s+)?[A-Za-z_][^;{}]*?\b' + name +
                         r'\s*\(', re.M | re.S)
    match = None
    for candidate in pattern.finditer(source):
        brace_candidate = source.find('{', candidate.end())
        semicolon = source.find(';', candidate.end())
        if brace_candidate >= 0 and (semicolon < 0 or brace_candidate < semicolon):
            match = candidate
            break
    if match is None:
        raise ValueError(name)
    brace = source.index('{', match.end())
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


xhci = (ROOT / 'kernel/src/usb/xhci.c').read_text()
port = (ROOT / 'kernel/src/usb/xhci_port.c').read_text()
production = ''.join(function(xhci, name) for name in (
    'xhci_class_probe_result', 'xhci_class_binding_result',
    'xhci_device_class_name', 'xhci_device_setup_finish'))
production += function(port, 'xhci_process_deferred_port_change')

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint64_t u64; typedef size_t usize;
typedef enum {
 XHCI_SUCCESS=0, XHCI_ERR_NO_MEMORY=1, XHCI_ERR_TIMEOUT=2,
 XHCI_ERR_INVALID_PARAM=3, XHCI_ERR_NOT_SUPPORTED=4,
 XHCI_ERR_HW_HALTED=5, XHCI_ERR_CONTROLLER_BAD=6,
 XHCI_ERR_PORT_RESET_FAIL=7, XHCI_ERR_DEVICE_TIMEOUT=8,
 XHCI_ERR_TRANSACTION=9
} xhci_status_t;
typedef enum { XHCI_PORT_DISCONNECTED, XHCI_PORT_CONNECTED_OBSERVED,
 XHCI_PORT_DEBOUNCING, XHCI_PORT_RESET_REQUESTED,
 XHCI_PORT_RESET_IN_PROGRESS, XHCI_PORT_RESET_COMPLETED,
 XHCI_PORT_ENABLED, XHCI_PORT_ENUMERATING, XHCI_PORT_READY,
 XHCI_PORT_FAILED } xhci_port_state_t;
typedef enum { XHCI_DEVICE_NO_SLOT, XHCI_DEVICE_SLOT_ENABLED,
 XHCI_DEVICE_ADDRESSING, XHCI_DEVICE_ADDRESSED, XHCI_DEVICE_EP0_READY,
 XHCI_DEVICE_DESCRIPTORS_READY, XHCI_DEVICE_ENDPOINTS_CONFIGURED,
 XHCI_DEVICE_USB_CONFIGURED, XHCI_DEVICE_CLASS_ATTACHING,
 XHCI_DEVICE_READY, XHCI_DEVICE_FAILED } xhci_device_state_t;
typedef struct xhci_device {
 u8 slot_id, class_flags; bool class_ready, setup_finished;
 xhci_status_t setup_result; xhci_device_state_t state;
} xhci_device_t;
typedef struct { xhci_port_state_t state; xhci_status_t result; } port_record_t;
typedef struct xhci_controller {
 bool last_setup_retry_safe; port_record_t ports[256];
} xhci_controller_t;
#define XHCI_DEVICE_CLASS_HID 1U
#define XHCI_DEVICE_CLASS_STORAGE 2U
#define XHCI_DEVICE_CLASS_HUB 4U
#define XHCI_USB_DEVICE_ID_BASE 0x4000000000000000ULL
#define XHCI_PORTSC_CCS 1U
#define XHCI_PORTSC_PR (1U<<4)
#define XHCI_PORTSC_WPR (1U<<31)
#define XHCI_PORTSC_SPEED(v) ((u8)(((v) >> 10) & 15U))
#define XHCI_DEBUG_LOG(...) ((void)0)
#define MG_EVENT_CLASS_DEVICE 1U
#define MG_EVENT_CLASS_USB 2U
#define MG_EVENT_USB_DEVICE_ADDED 3U
static unsigned cleanup_calls, teardown_calls;
static xhci_status_t next_setup_result;
static xhci_device_t *state_device;
static volatile u32 portsc[256];
static char log_text[256];
static void xhci_queue_device_teardown(xhci_controller_t *x, u8 slot, bool event)
{ (void)x; (void)slot; (void)event; teardown_calls++; }
static bool xhci_cleanup_failed_device(xhci_controller_t *x, xhci_device_t *d)
{ (void)x; (void)d; cleanup_calls++; return true; }
static void xhci_set_device_state(xhci_controller_t *x, u8 slot,
                                  xhci_device_state_t state, const char *why)
{ (void)x; (void)why; if(state_device && state_device->slot_id==slot) state_device->state=state; }
static xhci_status_t xhci_setup_finish(xhci_controller_t *x,
                                       xhci_status_t result, bool owns)
{ (void)x; (void)owns; return result; }
static volatile u32 *xhci_get_portsc_ptr(xhci_controller_t *x, u8 port)
{ (void)x; return port ? &portsc[port] : NULL; }
static void xhci_set_port_state(xhci_controller_t *x, u8 port,
                                xhci_port_state_t state, u32 raw,
                                const char *reason)
{ (void)raw; (void)reason; x->ports[port].state = state; }
static void xhci_set_port_result(xhci_controller_t *x, u8 port,
                                 xhci_status_t result, const char *reason)
{ (void)reason; x->ports[port].result = result; }
static bool xhci_detach_device(xhci_controller_t *x, u8 port)
{ (void)x; (void)port; return true; }
static bool xhci_abandon_device(xhci_controller_t *x, u8 port)
{ (void)x; (void)port; return true; }
static u8 xhci_device_slot_for_port(xhci_controller_t *x, u8 port)
{ (void)x; (void)port; return 0; }
static xhci_status_t xhci_reset_port(xhci_controller_t *x, u8 port,
                                     volatile u32 *register_value)
{ (void)x; (void)port; (void)register_value; return XHCI_SUCCESS; }
static xhci_status_t xhci_setup_device(xhci_controller_t *x, u8 port, u8 speed)
{ (void)x; (void)port; (void)speed; return next_setup_result; }
static bool xhci_setup_retry_allowed(xhci_controller_t *x)
{ (void)x; return false; }
static void ipc_publish_event(u32 classes, u32 event, u64 id, const char *name)
{ (void)classes; (void)event; (void)id; (void)name; }
static u64 xhci_device_instance_generation(xhci_controller_t *x, u8 slot)
{ (void)x; (void)slot; return 1; }
static const char *xhci_usb_device_event_name(xhci_controller_t *x, u8 slot)
{ (void)x; (void)slot; return "USB device"; }
static void kprint(const char *format, ...)
{ va_list args; va_start(args, format); vsnprintf(log_text, sizeof(log_text), format, args); va_end(args); }
'''

checks = r'''
int main(void)
{
 bool supported=true;
 assert(xhci_class_probe_result(XHCI_SUCCESS, &supported)==XHCI_SUCCESS && supported);
 assert(xhci_class_probe_result(XHCI_ERR_NOT_SUPPORTED, &supported)==XHCI_SUCCESS && !supported);
 const xhci_status_t errors[]={XHCI_ERR_INVALID_PARAM, XHCI_ERR_CONTROLLER_BAD,
  XHCI_ERR_NO_MEMORY, XHCI_ERR_TIMEOUT, XHCI_ERR_TRANSACTION};
 for (unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
  supported=true;
  assert(xhci_class_probe_result(errors[i], &supported)==errors[i] && !supported);
 }
 assert(xhci_class_probe_result(XHCI_SUCCESS, NULL)==XHCI_ERR_INVALID_PARAM);

 /* A fully enumerated class miss is ready and non-error. A matched class
    that fails attachment and any low-level status remain failures. */
 xhci_controller_t controller={0};
 xhci_device_t device={.slot_id=1, .class_ready=false};
 state_device=&device;
 assert(strcmp(xhci_device_class_name(0), "unsupported")==0);
 assert(strcmp(xhci_device_class_name(XHCI_DEVICE_CLASS_HID), "hid")==0);
 assert(xhci_class_binding_result(0, false)==XHCI_SUCCESS);
 assert(xhci_class_binding_result(XHCI_DEVICE_CLASS_HID, false)==XHCI_ERR_TRANSACTION);
 assert(xhci_class_binding_result(XHCI_DEVICE_CLASS_HID, true)==XHCI_SUCCESS);
 assert(xhci_device_setup_finish(&controller, &device, XHCI_SUCCESS, false)==XHCI_SUCCESS);
 assert(device.state==XHCI_DEVICE_READY && device.setup_result==XHCI_SUCCESS);
 assert(cleanup_calls==0 && teardown_calls==0);
 for (unsigned i=0;i<2;i++) {
  xhci_status_t error=i?XHCI_ERR_INVALID_PARAM:XHCI_ERR_CONTROLLER_BAD;
  device=(xhci_device_t){.slot_id=1, .class_ready=false};
  assert(xhci_device_setup_finish(&controller, &device, error, false)==error);
  assert(device.state==XHCI_DEVICE_FAILED && device.setup_result==error);
 }
 device=(xhci_device_t){.slot_id=1,.class_flags=XHCI_DEVICE_CLASS_HID,
                        .class_ready=false};
 assert(xhci_device_setup_finish(&controller, &device, XHCI_SUCCESS, false)==
        XHCI_ERR_TRANSACTION);
 assert(device.state==XHCI_DEVICE_FAILED && device.setup_result==XHCI_ERR_TRANSACTION);

 /* Runtime diagnostics remain visible for the two reported status codes,
    while successful enumeration without a class match emits no error. */
 const xhci_status_t runtime_results[]={XHCI_ERR_CONTROLLER_BAD,
                                        XHCI_ERR_INVALID_PARAM};
 for (unsigned i=0;i<2;i++) {
  memset(&controller,0,sizeof(controller)); memset((void*)portsc,0,sizeof(portsc));
  log_text[0]='\0'; portsc[7]=XHCI_PORTSC_CCS | (3U<<10);
  next_setup_result=runtime_results[i];
  xhci_process_deferred_port_change(&controller,7);
  char expected[64]; snprintf(expected,sizeof(expected),"Code: %d",runtime_results[i]);
  assert(controller.ports[7].state==XHCI_PORT_FAILED);
  assert(controller.ports[7].result==runtime_results[i]);
  assert(strstr(log_text, expected)!=NULL);
 }
 memset(&controller,0,sizeof(controller)); memset((void*)portsc,0,sizeof(portsc));
 log_text[0]='\0'; portsc[6]=XHCI_PORTSC_CCS | (3U<<10);
 next_setup_result=XHCI_SUCCESS;
 xhci_process_deferred_port_change(&controller,6);
 assert(controller.ports[6].state==XHCI_PORT_READY);
 assert(controller.ports[6].result==XHCI_SUCCESS && log_text[0]=='\0');
 puts("xHCI USB support policy: unsupported class is quiet; controller/parameter failures remain failed and logged");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-xhci-usb-support-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    c.write_text(fixture + production + checks)
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'gcc',
                    '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(c), '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
