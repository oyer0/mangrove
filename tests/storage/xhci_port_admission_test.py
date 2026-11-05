#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production port reset admission and terminal-state event handling."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/usb/xhci_port.c').read_text()

def function(name):
    match = re.search(r'^(?:static )?(?:void|xhci_status_t) ' + name + r'\(', source, re.M)
    start = source.index('{', match.end())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'

fixture = r'''
#include <assert.h>
#include <string.h>
#include <xhci_regs.h>
#include <xhci_trb.h>
typedef enum { XHCI_PORT_DISCONNECTED, XHCI_PORT_CONNECTED_OBSERVED,
 XHCI_PORT_DEBOUNCING, XHCI_PORT_RESET_REQUESTED, XHCI_PORT_RESET_IN_PROGRESS,
 XHCI_PORT_RESET_COMPLETED, XHCI_PORT_ENABLED, XHCI_PORT_ENUMERATING,
 XHCI_PORT_READY, XHCI_PORT_FAILED } xhci_port_state_t;
typedef int xhci_status_t;
#define XHCI_SUCCESS 0
#define XHCI_ERR_NOT_SUPPORTED 4
#define XHCI_ERR_PORT_RESET_FAIL 7
#define XHCI_SPEED_UNKNOWN 0
#define XHCI_SPEED_SUPER 4
#define XHCI_PORT_RESET_TIMEOUT_MS 500U
#define XHCI_PORT_DEBOUNCE_MS 100U
#define XHCI_PORT_POLL_MS 1U
#define XHCI_DEBUG_LOG(...) ((void)0)
typedef struct {xhci_port_state_t state;int result;const char *reason;} xhci_controller_t;
static u32 portsc;
static unsigned writes,queued;
static u64 now;
static bool complete_reset;
static volatile u32 *xhci_get_portsc_ptr(xhci_controller_t*x,u8 p){(void)x;assert(p==1);return &portsc;}
static xhci_port_state_t xhci_get_port_state(xhci_controller_t*x,u8 p){assert(p==1);return x->state;}
static void xhci_set_port_state(xhci_controller_t*x,u8 p,xhci_port_state_t s,u32 raw,const char*r){
 assert(p==1);(void)raw;x->state=s;x->reason=r;
}
static void xhci_set_port_result(xhci_controller_t*x,u8 p,int rc,const char*r){assert(p==1);x->result=rc;x->reason=r;}
static void xhci_write_portsc(volatile u32*p,u32 flags){writes++;*p|=flags;}
static void xhci_clear_portsc_bit(volatile u32*p,u32 bits){writes++;*p&=~bits;}
static void xhci_port_reset_log(u8 p,xhci_port_state_t s,u32 raw,const char*r){(void)p;(void)s;(void)raw;(void)r;}
static void xhci_diag_timeline_at(const char*s,uintptr_t p,u32 raw){(void)s;(void)p;(void)raw;}
static u64 timer_uptime_ms(void){return now;}
static void timer_sleep(u64 ms){
 now+=ms;
 if(complete_reset && (portsc&XHCI_PORTSC_PR)){
  portsc&=~(XHCI_PORTSC_PR|XHCI_PORTSC_PLS_MASK);
  portsc|=XHCI_PORTSC_PED|XHCI_PORTSC_PRC;
 }
}
static bool xhci_is_busy(xhci_controller_t*x){(void)x;return false;}
static void xhci_queue_port_change(xhci_controller_t*x,u8 p,const char*r){(void)x;(void)r;assert(p==1);queued++;}
'''
checks = r'''
int main(void){
 xhci_controller_t x={0};
 /* Provisional X250 transcription: connected, powered, FS, Inactive, no CSC. */
 portsc=0x6c1;
 assert((portsc&XHCI_PORTSC_CCS) && !(portsc&XHCI_PORTSC_PED));
 assert(((portsc&XHCI_PORTSC_PLS_MASK)>>5)==6 && !(portsc&XHCI_PORTSC_CSC));
 assert(xhci_reset_port(&x,1,&portsc)==XHCI_ERR_NOT_SUPPORTED);
 assert(x.state==XHCI_PORT_FAILED && strcmp(x.reason,"connected-link-not-enumerable")==0);
 assert(portsc==0x6c1 && writes==0 && now==0);
 /* CCS is insufficient for any of these invalid/ambiguous reset entries. */
 const u32 invalid[]={0x6c1,0x6a1,0x641,0x601,0x6e1&~XHCI_PORTSC_PP,
                      0x6e1|XHCI_PORTSC_PR,0x6e1|XHCI_PORTSC_WPR};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
  portsc=invalid[i];assert(xhci_reset_port(&x,1,&portsc)==XHCI_ERR_NOT_SUPPORTED);
  assert(portsc==invalid[i] && writes==0 && now==0);
 }
 /* Normal USB2 Disabled maps to Polling, and needs the existing reset. */
 portsc=0x6e1;complete_reset=true;
 assert(xhci_port_link_enumerable(portsc));
 assert(xhci_reset_port(&x,1,&portsc)==XHCI_SUCCESS);
 assert(x.state==XHCI_PORT_ENABLED && (portsc&XHCI_PORTSC_PED));
 assert(now>=100 && writes>0);
 assert(xhci_port_link_enumerable(0x1203)); /* powered, SS enabled U0 */
 assert(!xhci_port_link_enumerable(0x2a0)); /* disconnected */
 /* A non-CSC completion event must preserve terminal setup/failure history. */
 xhci_trb_t event={.param1=1U<<24};portsc=0x603|XHCI_PORTSC_PLC;
 x.state=XHCI_PORT_READY;xhci_handle_port_status_change(&x,&event);
 assert(x.state==XHCI_PORT_READY && !queued);
 x.state=XHCI_PORT_FAILED;portsc=0x6c1;xhci_handle_port_status_change(&x,&event);
 assert(x.state==XHCI_PORT_FAILED && !queued);
 /* A new connection generation can be admitted; reset completion cannot. */
 portsc=0x6e1|XHCI_PORTSC_CSC;xhci_handle_port_status_change(&x,&event);
 assert(x.state==XHCI_PORT_CONNECTED_OBSERVED && queued==1);
 x.state=XHCI_PORT_ENUMERATING;portsc=0x603|XHCI_PORTSC_CSC;
 xhci_handle_port_status_change(&x,&event);assert(x.state==XHCI_PORT_ENUMERATING && queued==1);
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-xhci-port-admission-') as tmp:
    c, binary = Path(tmp)/'test.c', Path(tmp)/'test'
    c.write_text(fixture + function('xhci_reset_port') +
                 function('xhci_handle_port_status_change') + checks)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc', '-std=c11',
                    '-Wall', '-Wextra', '-Werror', '-Iinclude', '-Ikernel/include/usb',
                    str(c), '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: Inactive/invalid links cause no reset or delay, valid USB2 resets, terminal events do not retry')
