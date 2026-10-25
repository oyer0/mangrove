#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production boot-only GOP mapping, PCI eligibility and alias exclusion."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[2]


def function(path, name):
    source = (ROOT / path).read_text()
    pattern = r'^(?:static )?[A-Za-z_][^;{}]*?\b' + name + r'\([^;{}]*\)\s*\{'
    match = re.search(pattern, source, re.M)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;typedef uint32_t u32;typedef uint16_t u16;typedef uint8_t u8;
typedef u64 phys_addr_t;typedef u64 virt_addr_t;
#define VMM_PAGE_SIZE 4096ULL
#define PHYS_MAP_LIMIT (1ULL<<46)
#define IOREMAP_BASE 0xffffc08000000000ULL
#define IOREMAP_LIMIT 0xffffc10000000000ULL
#define PTE_FRAME_MASK 0x000ffffffffff000ULL
#define PTE_PRESENT 1ULL
#define PTE_READWRITE 2ULL
#define PTE_HUGE 128ULL
#define PTE_NX (1ULL<<63)
#define PCI_COMMAND_MEMORY 2U
typedef struct {u64 entries[512];} __attribute__((aligned(4096))) page_table_t;
typedef struct {u64 FramebufferPhysicalBase,FramebufferSize;u32 FramebufferWidth,FramebufferHeight,PixelsPerScanLine;} BOOT_INFO;
static page_table_t root,pdpt,pd,pt,*kernel_pml4=&root;
static u64 framebuffer_first,framebuffer_limit,framebuffer_virtual,framebuffer_cache_flags;
static u64 ioremap_next,observed_cr3;
static bool interrupts,wc_supported;
static unsigned cpu_count,held,maps,publishes,wc_queries;
static int fail_after;
static u64 map_flags[16],map_phys[16];
static u32 config[8][64];
static u64 spin_lock_irqsave(unsigned*l){assert(!*l);*l=1;return 0;}
static void spin_unlock_irqrestore(unsigned*l,u64 f){(void)f;assert(*l);*l=0;}
#define vmm_kernel_lock held
static bool vmm_interrupts_enabled(void){return interrupts;}
static unsigned cpu_online_count(void){return cpu_count;}
static u64 vmm_table_phys(page_table_t*p){return (u64)(uintptr_t)p;}
static page_table_t*vmm_table_from_phys(u64 p){return (page_table_t*)(uintptr_t)(p&PTE_FRAME_MASK);}
static u64 x86_cache_uc_flags(void){return 24;}
static bool x86_cache_framebuffer_wc(u64 p,u64 n,u64*f){assert(!(p&4095)&&!(n&4095));wc_queries++;if(!wc_supported)return false;*f=128;return true;}
static void vmm_publish_kernel_mappings(void){assert(!held);publishes++;}
static u32 pci_read32(u8 bus,u8 slot,u8 fn,u8 off){if(bus||slot!=2)return ~0U;return config[fn][off/4];}
static u16 pci_read16(u8 b,u8 s,u8 f,u8 o){return pci_read32(b,s,f,o)>>(8*(o&2));}
'''
body = function('kernel/src/pci/pci.c', 'pci_boot_framebuffer_prefetchable')
body += function('kernel/src/vmm.c', 'vmm_framebuffer_page')
body += r'''
static bool vmm_map_kernel_one(page_table_t*r,void*v,u64 p,u64 flags){
 assert(held&&r==kernel_pml4);
 if(vmm_framebuffer_page(p)&&((u64)(uintptr_t)v!=framebuffer_virtual+p-framebuffer_first||(flags&152)!=framebuffer_cache_flags))return false;
 if(fail_after==0)return false;if(fail_after>0)fail_after--;
 assert(maps<16);map_flags[maps]=flags;map_phys[maps++]=p;return true;
}
'''
body += function('kernel/src/vmm.c', 'vmm_physical_range_unused')
context = function('kernel/src/vmm.c', 'vmm_boot_framebuffer_context')
context = re.sub(r'__asm__ volatile\([^;]*;', 'current = observed_cr3;', context)
body += context
body += function('kernel/src/vmm.c', 'vmm_map_boot_framebuffer')
body += function('kernel/src/vmm.c', 'vmm_map_mmio')
suffix = r'''
static BOOT_INFO info;
static void reset(void){
 memset(&root,0,sizeof(root));memset(&pdpt,0,sizeof(pdpt));memset(&pd,0,sizeof(pd));memset(&pt,0,sizeof(pt));
 for(unsigned f=0;f<8;f++)for(unsigned i=0;i<64;i++)config[f][i]=~0U;
 config[0][0]=0x11111234;config[0][1]=2;config[0][2]=0x03000000;config[0][3]=0;
 for(unsigned i=4;i<10;i++)config[0][i]=0;
 config[0][4]=0x80000008;
 framebuffer_first=framebuffer_limit=framebuffer_virtual=framebuffer_cache_flags=0;
 ioremap_next=IOREMAP_BASE;observed_cr3=0x100000;cpu_count=1;
 interrupts=false;wc_supported=true;maps=publishes=held=wc_queries=0;fail_after=-1;
 info=(BOOT_INFO){0x80000000,8192,128,16,128};
}
static void connect_tables(void){
 root.entries[0]=(u64)(uintptr_t)&pdpt|3;pdpt.entries[0]=(u64)(uintptr_t)&pd|3;pd.entries[0]=(u64)(uintptr_t)&pt|3;
}
int main(void){bool wc;
 reset();assert(pci_boot_framebuffer_prefetchable(0x80000000));
 config[0][4]&=~8U;assert(!pci_boot_framebuffer_prefetchable(0x80000000));
 reset();assert(!pci_boot_framebuffer_prefetchable(0x80001000));
 config[0][1]=0;assert(!pci_boot_framebuffer_prefetchable(0x80000000));
 reset();config[0][2]=0x02000000;assert(!pci_boot_framebuffer_prefetchable(0x80000000));
 reset();config[0][4]|=1;assert(!pci_boot_framebuffer_prefetchable(0x80000000));
 reset();config[0][4]=0x8000000c;config[0][5]=1;assert(pci_boot_framebuffer_prefetchable(0x180000000));
 reset();config[0][4]=0;config[0][9]=0x8000000c;assert(!pci_boot_framebuffer_prefetchable(0x80000000));
 reset();memcpy(config[3],config[0],sizeof(config[0]));config[0][4]=0;
 assert(!pci_boot_framebuffer_prefetchable(0x80000000));config[0][3]=0x00800000;
 assert(pci_boot_framebuffer_prefetchable(0x80000000));
 reset();config[0][3]=0x10000;assert(!pci_boot_framebuffer_prefetchable(0x80000000));
 reset();assert(vmm_map_boot_framebuffer(&info,&wc)==(void*)IOREMAP_BASE&&wc);
 assert(maps==2&&!publishes&&map_phys[1]==0x80001000);
 assert(map_flags[0]==(PTE_READWRITE|PTE_NX|128));
 assert(!vmm_map_boot_framebuffer(&info,&wc)&&!wc);
 assert(!vmm_map_mmio(info.FramebufferPhysicalBase,4096));
 reset();assert(vmm_map_mmio(0xfee00000,4096)==(void*)IOREMAP_BASE);
 assert(map_flags[0]==(PTE_READWRITE|PTE_NX|24)&&publishes==1);
 reset();wc_supported=false;assert(vmm_map_boot_framebuffer(&info,&wc)&&!wc);assert((map_flags[0]&152)==24);
 reset();config[0][4]&=~8U;assert(vmm_map_boot_framebuffer(&info,&wc)&&!wc&&!wc_queries);
 reset();info.FramebufferPhysicalBase+=16;assert(vmm_map_boot_framebuffer(&info,&wc)==(void*)(IOREMAP_BASE+16)&&!wc&&maps==3);
 reset();info.FramebufferSize++;assert(vmm_map_boot_framebuffer(&info,&wc)&&!wc&&maps==3);
 reset();info.FramebufferSize--;assert(!vmm_map_boot_framebuffer(&info,&wc)&&!maps);
 reset();info.FramebufferHeight=0;assert(!vmm_map_boot_framebuffer(&info,&wc));
 reset();info.PixelsPerScanLine=127;assert(!vmm_map_boot_framebuffer(&info,&wc));
 reset();info.FramebufferSize=UINT64_MAX;assert(!vmm_map_boot_framebuffer(&info,&wc));
 reset();info.FramebufferPhysicalBase=PHYS_MAP_LIMIT;assert(!vmm_map_boot_framebuffer(&info,&wc));
 reset();interrupts=true;assert(!vmm_map_boot_framebuffer(&info,&wc)&&!maps&&!held);
 reset();observed_cr3=vmm_table_phys(&root);assert(!vmm_map_boot_framebuffer(&info,&wc)&&!maps&&!held);
 reset();cpu_count=2;assert(!vmm_map_boot_framebuffer(&info,&wc)&&!maps&&!held);
 for(unsigned level=1;level<=3;level++){
  reset();connect_tables();if(level==1)pt.entries[0]=0x80001003;
  if(level==2)pd.entries[0]=0x80000083;if(level==3)pdpt.entries[0]=0x80000083;
  assert(!vmm_map_boot_framebuffer(&info,&wc)&&!maps&&ioremap_next==IOREMAP_BASE);
 }
 reset();connect_tables();pt.entries[0]=0x80002003;
 assert(vmm_map_boot_framebuffer(&info,&wc)&&wc); /* Adjacent page is not an alias. */
 reset();fail_after=1;assert(!vmm_map_boot_framebuffer(&info,&wc)&&!wc);
 assert(maps==1&&framebuffer_limit&&ioremap_next==IOREMAP_BASE+8192);
 fail_after=-1;assert(!vmm_map_boot_framebuffer(&info,&wc));
 puts("framebuffer mapping: prefetchable GOP eligibility, UC MMIO/fallback, all-level aliases, boot context and partial failure passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-gop-') as tmp:
    cfile=Path(tmp)/'mapping.c';binary=Path(tmp)/'mapping';cfile.write_text(prefix+body+suffix)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc','-std=gnu17','-Wall','-Wextra','-Werror',
                    '-Wno-misleading-indentation','-fsanitize=address,undefined',str(cfile),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},check=True)
