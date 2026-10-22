#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production BOT status/retry policy; USB transfers and DMA are mocked."""
from pathlib import Path
import subprocess
import sys
import tempfile
import re
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/src/usb/xhci_storage.c').read_text()
def function(name):
    for m in re.finditer(r'^static [^\n;{}]*\b'+name+r'\(',s,re.M):
        b=s.index('{',m.end());end=b+1;depth=1
        while depth:
            depth+=(s[end]=='{')-(s[end]=='}');end+=1
        return s[m.start():end]+'\n'
    raise ValueError(name)
pre=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uintptr_t usize;
typedef struct {void*xhc;u8 slot_id,bulk_in_dci,bulk_out_dci;u32 tag;bool scsi_failure;} usb_mass_storage_device_t;
'''
structs=s[s.index('typedef struct __attribute__((packed)) {'):s.index('static usb_mass_storage_device_t devices')]
model=r'''
static unsigned mode,bulk_calls,alloc_calls,frees,retries;
static bool live;static u32 last_tag;static u8 opcode;
static void*xhci_dma_alloc(usize size,uintptr_t*p){alloc_calls++;if(mode==5&&alloc_calls==2)return NULL;void*v=calloc(1,size);*p=(uintptr_t)v;return v;}
static void xhci_dma_free(void*v,usize size){(void)size;if(v){frees++;free(v);}}
static bool xhci_slot_accepts_io(void*x,u8 s){(void)x;(void)s;return live;}
static void xhci_quarantine_slot(void*x,u8 s){(void)x;(void)s;live=false;}
static bool bulk_transfer(usb_mass_storage_device_t*d,u8 ep,void*buf,uintptr_t p,u32 n,bool in,bool short_ok)
{(void)d;(void)ep;(void)p;(void)short_ok;bulk_calls++;
 if(!in&&n==sizeof(usb_cbw_t)){usb_cbw_t*c=buf;last_tag=c->tag;opcode=c->cb[0];
  if(mode==1){live=false;return false;}return true;}
 if(in&&n==sizeof(usb_csw_t)){usb_csw_t*c=buf;c->signature=mode==2?0:0x53425355;
 c->tag=last_tag;c->status=(mode==3&&opcode!=3&&retries==0)?1:0;
 if(mode==4){c->status=2;}
 return true;}
 if(in&&opcode==3){((u8*)buf)[2]=6;return true;}return true;}
static void kprint(const char*p,...){(void)p;}
static void bot_retry_backoff(void){retries++;}
'''
sense=s[s.index('typedef struct {\n    bool valid;'):s.index('static bool bot_request_sense')]
post=r'''
int main(void){usb_mass_storage_device_t d={.slot_id=1,.bulk_in_dci=3,.bulk_out_dci=2};u8 cdb[6]={0x12};
 for(mode=0;mode<6;mode++){bulk_calls=alloc_calls=frees=retries=0;live=true;
 bool ok=bot_command_with_sense_retry(&d,cdb,6,NULL,0,0,false);
 assert(ok==(mode==0||mode==3));assert(alloc_calls-(mode==5?1:0)==frees);
 if(mode==1)assert(bulk_calls==1&&retries==0&&!live);
 if(mode==2||mode==4)assert(bulk_calls==2&&retries==0&&!live);
 if(mode==3)assert(retries==1&&bulk_calls==7&&live);
 if(mode==5)assert(!bulk_calls&&frees==1);
 }
 puts("PASS: BOT completed success, uncertain transport, invalid CSW, phase error, UNIT ATTENTION retry, allocation cleanup");}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-bot-') as t:
 p=Path(t)/'test.c';p.write_text(pre+structs+model+function('bot_command')+sense+function('bot_request_sense')+function('bot_command_with_sense_retry')+post)
 subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
