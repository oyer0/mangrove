#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read production PORTSC snapshots and protocol ranges without MMIO writes."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/src/usb/xhci.c').read_text()
def function(name):
    m=re.search(r'^void '+name+r'\(',s,re.M)
    start=s.index('{',m.end());end=start+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[m.start():end]+'\n'
a=s.index('typedef struct {\n    u64 ms;')
b=s.index('static xhci_boot_ports_sample_t boot_ports_samples[5];',a)+len('static xhci_boot_ports_sample_t boot_ports_samples[5];')
fixture=r'''
#include <assert.h>
#include <string.h>
#include <xhci_regs.h>
#define XHCI_MAX_EXT_CAPS 50U
#define XHCI_PORT_FAILED 2U
typedef struct {
 uintptr_t mmio_base;u64 mmio_phys_base;u32 max_ports;
 xhci_cap_regs_t *cap_regs;xhci_op_regs_t *op_regs;
 struct {bool device_enumerated;unsigned state;} ports[256];
} xhci_controller_t;
static _Alignas(16) u8 mmio[65536];
static u64 now;
static unsigned stages,protocols,raw_calls,last_connected;
static bool truncate_protocol;
static u32 raw_final[32];
static u64 timer_uptime_ms(void){return now++;}
static volatile u32 *xhci_get_portsc_ptr(xhci_controller_t*x,u8 port){
 assert(port&&port<=x->max_ports);return (volatile u32*)((u8*)x->op_regs+0x400+0x10*(port-1));
}
static volatile u32 *xhci_extended_capability(xhci_controller_t*x,u32 offset,u32 size){
 assert(xhci_ext_cap_range_valid(offset,size));assert(offset+size<=sizeof(mmio));
 if(truncate_protocol&&size==16)return NULL;
 return (volatile u32*)(x->mmio_base+offset);
}
static void x250_boot_trace_ports_stage(const char*n,u64 t,u32 c,u32 s,u32 connected,u32 power){
 (void)n;(void)t;assert(c==5&&s==0);assert(power==0x7fff);
 assert(connected==0||connected==(1U<<5));stages++;
}
static void x250_boot_trace_protocol(u32 major,u32 minor,u32 first,u32 count,u32 slot){
 assert(minor==0&&slot==0);
 if(protocols==0)assert(major==2&&first==1&&count==11);
 else assert(major==3&&first==12&&count==4);
 protocols++;
}
static void x250_boot_trace_ports_raw(u32 caplen,u64 physical,const u32*r,const u32*start,const u32*end,u32 count){
 assert(caplen==0x80&&physical==0xf1220480&&count==15);
 assert(r[5]==0x2a0&&start[5]==0x2a0&&end[5]==0x2a1);
 memcpy(raw_final,end,count*sizeof(u32));raw_calls++;
}
static unsigned xhci_storage_device_count(void){return 0;}
static void x250_boot_trace_xhci_summary(u32 ports,u32 connected,u32 enumerated,u32 failed,u32 msd){
 assert(ports==15&&enumerated==1&&failed==0&&msd==0);last_connected=connected;
}
'''
checks=r'''
int main(void){
 xhci_controller_t x={.mmio_base=(uintptr_t)mmio,.mmio_phys_base=0xf1220000,.max_ports=15,
 .cap_regs=(xhci_cap_regs_t*)mmio,.op_regs=(xhci_op_regs_t*)(mmio+0x80)};
 x.cap_regs->caplength=0x80;x.cap_regs->hccparams1=0x20000000;
 x.op_regs->usbcmd=5;
 for(unsigned port=1;port<=15;port++)*xhci_get_portsc_ptr(&x,port)=0x2a0;
 u32 *legacy=(u32*)(mmio+0x8000),*usb2=(u32*)(mmio+0x8020),*usb3=(u32*)(mmio+0x8040);
 legacy[0]=1|(8U<<8);usb2[0]=(2U<<24)|(8U<<8)|2;
 usb2[1]=usb3[1]=0x20425355;usb2[2]=1|(11U<<8);usb3[0]=(3U<<24)|2;usb3[2]=12|(4U<<8);
 for(unsigned stage=0;stage<4;stage++)xhci_trace_capture_ports(&x,stage);
 u64 before=now;xhci_trace_capture_ports(&x,5);assert(now==before);
 *xhci_get_portsc_ptr(&x,6)|=1;x.ports[6].device_enumerated=true;
 static u8 saved[sizeof(mmio)];memcpy(saved,mmio,sizeof(mmio));
 xhci_trace_report_ports(&x);
 assert(memcmp(saved,mmio,sizeof(mmio))==0); /* no reset, power or status writes */
 assert(stages==5&&protocols==2&&raw_calls==1&&last_connected==1&&raw_final[5]==0x2a1);
 assert(boot_ports_samples[1].connected==0&&boot_ports_samples[4].connected==(1U<<5));
 truncate_protocol=true;protocols=0;xhci_trace_report_ports(&x);assert(protocols==0);
 assert(memcmp(saved,mmio,sizeof(mmio))==0);
 unsigned before_calls=raw_calls;xhci_trace_report_ports(NULL);assert(raw_calls==before_calls);
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-xhci-port-samples-') as tmp:
    c=Path(tmp)/'test.c';binary=Path(tmp)/'test'
    c.write_text(fixture+s[a:b]+'\n'+function('xhci_trace_capture_ports')+function('xhci_trace_report_ports')+checks)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-std=c11','-Wall','-Wextra','-Werror',
                    '-Iinclude','-Ikernel/include/usb',str(c),'-o',str(binary)],cwd=ROOT,check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
print('PASS: read-only per-port snapshots, late attach, protocol ranges, physical offsets and truncated capability')
