#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot interrupt acknowledgement must not clear an undispatched port change."""
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/src/usb/xhci.c').read_text();a=s.index('void xhci_acknowledge_boot_interrupts(');b=s.index('{',a)+1;depth=1
while depth:
    depth+=(s[b]=='{')-(s[b]=='}');b+=1
fixture=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#define XHCI_USBSTS_EINT 8U
#define XHCI_USBSTS_PCD 16U
#define XHCI_IMAN_IP 1U
#define XHCI_IMAN_IE 2U
#define XHCI_PORTSC_RW1C_MASK (1U<<17)
#define XHCI_PORTSC_RW1CS_MASK ((1U<<17)|2U)
static void discard_log(const char*f,...){(void)f;}
#define XHCI_DEBUG_LOG(...) discard_log(__VA_ARGS__)
typedef struct {u32 usbsts;} op_regs_t;
typedef struct {u32 iman;u64 erdp;} xhci_intr_regs_t;
typedef struct {op_regs_t *op_regs;u32 max_ports;} xhci_controller_t;
static xhci_intr_regs_t interrupt={.iman=3,.erdp=0x1000};
static u32 portsc=(1U<<17)|0x2e1U;static unsigned port_reads;
static xhci_intr_regs_t *xhci_get_intr_regs(xhci_controller_t*x,u8 n){(void)x;assert(n==0);return &interrupt;}
static volatile u32 *xhci_get_portsc_ptr(xhci_controller_t*x,u8 p){(void)x;(void)p;port_reads++;return &portsc;}
'''
checks=r'''
int main(void){
 op_regs_t op={.usbsts=24};xhci_controller_t x={.op_regs=&op,.max_ports=1};
 /* Attach arrives after the last drain; its Event Ring entry is not dispatched. */
 xhci_acknowledge_boot_interrupts(&x);
 assert(port_reads==0&&portsc==((1U<<17)|0x2e1U));
 assert((interrupt.iman&XHCI_IMAN_IE)!=0);
 xhci_acknowledge_boot_interrupts(NULL);x.op_regs=NULL;xhci_acknowledge_boot_interrupts(&x);
 assert(port_reads==0);
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-xhci-boot-ack-') as tmp:
    c=Path(tmp)/'test.c';binary=Path(tmp)/'test';c.write_text(fixture+s[a:b]+checks)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: boot interrupt acknowledgement preserves a queued attach and its CSC')
