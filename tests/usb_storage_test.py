#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production read functions with mocked devices, DMA and allocation."""
from pathlib import Path
import tempfile
import sys
ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
work = tempfile.TemporaryDirectory(prefix="mangrove-storage-")
TMP = Path(work.name)
import subprocess
s=(ROOT / 'kernel/src/usb/xhci_storage.c').read_text();start=s.index('static bool storage_read(');brace=s.index('{',start);end=brace+1;depth=1
while depth:
    depth+=(s[end]=='{')-(s[end]=='}');end+=1
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;typedef size_t usize;
typedef struct {void*driver_data;u64 id;u32 sector_size;} block_device_t;
typedef struct {bool active;block_device_t block;u64 block_count;} usb_mass_storage_device_t;
typedef struct {void*virt;u64 phys;} dma_buffer_t;
static void *large;static u64 large_phys;static unsigned mode,calls,frees,allocs;static bool fail_command,remove_during_read;
static dma_buffer_t dma_alloc(usize size,usize alignment){if(mode==3)return(dma_buffer_t){0};large=aligned_alloc(alignment,size);assert(large);allocs++;large_phys=mode==2?0x19000:0x18000;return(dma_buffer_t){large,large_phys};}
static void dma_free(dma_buffer_t d){if(d.virt){frees++;free(d.virt);large=NULL;}}
static uintptr_t virt_to_phys(void *p){usize off=(u8*)p-(u8*)large;return large_phys+off+((mode==1&&off>=4096)?4096:0);}
static void*xhci_dma_alloc(usize size,uintptr_t*phys){*phys=4096;allocs++;return malloc(size);}
static void xhci_dma_free(void*p,usize size){(void)size;frees++;free(p);}
static bool xhci_dma_defer_release(void*v,uintptr_t p,usize s,bool h){(void)v;(void)p;(void)s;(void)h;return true;}
static bool xhci_dma_may_release(uintptr_t phys,usize size){(void)phys;(void)size;return true;}
static void storage_teardown_wakeup(usb_mass_storage_device_t*d){(void)d;}
static bool bot_command_with_sense_retry(usb_mass_storage_device_t*d,u8*cdb,usize len,void*data,uintptr_t phys,u32 bytes,bool in){(void)len;(void)phys;assert(in);calls++;if(remove_during_read)d->active=false;if(fail_command)return false;u32 lba=((u32)cdb[2]<<24)|((u32)cdb[3]<<16)|((u32)cdb[4]<<8)|cdb[5];u32 sectors=((u32)cdb[7]<<8)|cdb[8];assert(bytes==sectors*512);for(u32 i=0;i<bytes;i++)((u8*)data)[i]=(u8)(lba+i/512);return true;}
'''
post=r'''
int main(void){usb_mass_storage_device_t dev={.active=true,.block_count=1024};dev.block=(block_device_t){&dev,123,512};u8 out[32768];
for(mode=0;mode<4;mode++){calls=frees=allocs=0;assert(storage_read(&dev.block,100,64,out));assert(calls==(mode==0?1:8));assert(allocs==frees);for(unsigned i=0;i<32768;i++)assert(out[i]==(u8)(100+i/512));}
mode=0;calls=frees=allocs=0;assert(storage_read(&dev.block,100,17,out));assert(calls==1&&allocs==frees);
calls=frees=allocs=0;fail_command=true;assert(!storage_read(&dev.block,100,64,out));assert(allocs==frees);
fail_command=false;dev.active=true;remove_during_read=true;calls=frees=allocs=0;u8 longer[65536];assert(!storage_read(&dev.block,100,128,longer));assert(calls==1&&allocs==frees);remove_during_read=false;
 dev.active=false;assert(!storage_read(&dev.block,100,64,out));
puts("PASS: USB batch sizing, bytes, fragmented-memory fallback, boundary fallback, allocation failure, transfer failure cleanup, device removal");}
'''
(TMP / 'mangrove_usb_batch_test.c').write_text(pre+s[start:end]+post)
subprocess.run([CC,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(TMP / 'mangrove_usb_batch_test.c'),'-o',str(TMP / 'mangrove_usb_batch_test')],check=True)
subprocess.run([str(TMP / 'mangrove_usb_batch_test')],check=True)
