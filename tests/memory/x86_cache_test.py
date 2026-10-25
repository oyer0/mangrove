#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production PAT/MTRR policy with controlled privileged-instruction adapters."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/x86_cache.c').read_text()
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)


def replace_function(name, replacement):
    global source
    match = re.search(r'^(?:static )?(?:void|bool|u64) ' + name + r'\([^;]*?\)\n\{', source, re.M)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    source = source[:match.start()] + replacement + source[pos:]


replace_function('cache_cpuid', '''static void cache_cpuid(u32 leaf,u32*a,u32*b,u32*c,u32*d) {
 *a=*b=*c=*d=0;
 if(leaf==0){*a=1;*b=0x68747541U;*d=0x69746e65U;*c=0x444d4163U;}
 if(leaf==1)*d=features;
 if(leaf==0x80000000U)*a=0x80000008U;
 if(leaf==0x80000008U)*a=48;
}''')
replace_function('cache_cr0', 'static u64 cache_cr0(void) {return fake_cr0;}')
replace_function('cache_cr3', 'static u64 cache_cr3(void) {return (u64)(uintptr_t)&root;}')
replace_function('cache_boot_context', 'static bool cache_boot_context(void) {return !irq_enabled;}')
replace_function('cache_install_pat', 'static void cache_install_pat(u64 value) {installs++;if(!fail_readback)pat=value;}')
replace_function('x86_cache_activate_boot_root', 'void x86_cache_activate_boot_root(u64 p) {(void)p;}')
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;typedef uint32_t u32;
#define CPU_MAX_COUNT 256U
#define PTE_FRAME_MASK 0x000ffffffffff000ULL
#define PTE_PRESENT 1ULL
#define PTE_HUGE 128ULL
#define PTE_WRITETHROUGH 8ULL
#define PTE_CACHEDISABLE 16ULL
typedef struct {u64 entries[512];} __attribute__((aligned(4096))) page_table_t;
static page_table_t root, pdpt, pd, pt;
static u64 pat,cap,def,base[16],mask[16],fake_cr0;
static u32 features,current_cpu,installs;
static bool irq_enabled,fail_readback;
static void *phys_to_virt(u64 p){return (void*)(uintptr_t)p;}
static u32 cpu_current_index(void){return current_cpu;}
static u64 rdmsr(u32 n) {
 if(n==0x277){assert(features&(1U<<16));return pat;}
 assert(features&(1U<<12));
 if(n==0xfe)return cap;
 if(n==0x2ff)return def;
 assert(n>=0x200&&n<0x220);
 return n&1?mask[(n-0x200)/2]:base[(n-0x200)/2];
}
'''
suffix = r'''
static void reset(void) {
 memset(&root,0,sizeof(root));memset(&pdpt,0,sizeof(pdpt));
 memset(&pd,0,sizeof(pd));memset(&pt,0,sizeof(pt));
 root.entries[0]=(u64)(uintptr_t)&pdpt|3;pdpt.entries[0]=(u64)(uintptr_t)&pd|3;
 pd.entries[0]=(u64)(uintptr_t)&pt|3;pt.entries[0]=0x100003;
 pat=0x0007040600070406ULL;cap=0x508;def=0xc06;
 memset(base,0,sizeof(base));memset(mask,0,sizeof(mask));
 base[0]=0x80000000;mask[0]=0xffff80000800ULL;
 features=(1U<<12)|(1U<<16)|(1U<<25)|(1U<<26);current_cpu=0;installs=0;
 fake_cr0=0;irq_enabled=fail_readback=false;
 cache_initialized=cache_wc_available=false;cache_uc_flags=24;
 cache_framebuffer_size=cache_framebuffer_start=0;
 memset(cache_cpu_valid,0,sizeof(cache_cpu_valid));
}
int main(void) {
 u64 f=0;
 reset();assert(x86_cache_init_bsp());assert(installs==1);
 assert(pat==0x0007040100070406ULL);assert(x86_cache_uc_flags()==24);
 assert(x86_cache_framebuffer_wc(0x80000000,4096000,&f)&&f==128);
 assert(!x86_cache_framebuffer_wc(0x80001000,4096,&f));
 assert(!x86_cache_framebuffer_wc(0x80000001,4096,&f));
 assert(!x86_cache_framebuffer_wc(0x80000000,1,&f));
 assert(!x86_cache_framebuffer_wc(UINT64_MAX-4095,4096,&f));
 assert(!x86_cache_framebuffer_wc(0,4096,&f));
 current_cpu=1;pat=0x0007040600070406ULL;
 assert(x86_cache_init_ap(1)&&x86_cache_cpu_ready(1));
 assert(cache_cpu_pat[1]==cache_cpu_pat[0]&&installs==2);
 assert(!x86_cache_init_ap(1)&&installs==2);
 current_cpu=2;pat^=0x100;assert(!x86_cache_init_ap(2));assert(!x86_cache_cpu_ready(2));
 pat=cache_expected_pat;base[0]+=4096;assert(!x86_cache_init_ap(2));
 reset();features&=~(1U<<16);assert(x86_cache_init_bsp());assert(!installs);
 assert(x86_cache_uc_flags()==24&&!x86_cache_framebuffer_wc(0x80000000,4096,&f));
 current_cpu=1;assert(x86_cache_init_ap(1));
 reset();features&=~(1U<<12);assert(x86_cache_init_bsp());assert(!installs);
 reset();cap&=~(1ULL<<10);assert(x86_cache_init_bsp());assert(!installs);
 reset();features&=~(1U<<26);assert(x86_cache_init_bsp());assert(!installs);
 reset();def&=~(1ULL<<11);assert(x86_cache_init_bsp());assert(!installs);
 reset();fake_cr0=1ULL<<30;assert(x86_cache_init_bsp());assert(!installs);
 reset();cap=(cap&~255ULL)|17;assert(x86_cache_init_bsp());assert(!installs);
 reset();pt.entries[0]|=128;assert(x86_cache_init_bsp());assert(!installs);
 reset();pd.entries[0]=0x200000|0x1083;assert(x86_cache_init_bsp());assert(!installs);
 reset();pdpt.entries[0]=0x40000000|0x1083;assert(x86_cache_init_bsp());assert(!installs);
 reset();pat=(pat&~0xff000000ULL)|0x06000000ULL;
 /* Strict UC at selector 7 is still usable; UC- at selector 2 is insufficient. */
 assert(x86_cache_init_bsp());assert(x86_cache_uc_flags()==(128|24));
 reset();pat=0x0606060606060606ULL;assert(!x86_cache_init_bsp());
 reset();fail_readback=true;assert(!x86_cache_init_bsp());
 reset();irq_enabled=true;assert(!x86_cache_init_bsp());
 reset();assert(x86_cache_init_bsp());irq_enabled=true;
 assert(!x86_cache_framebuffer_wc(0x80000000,4096,&f));
 reset();assert(x86_cache_init_bsp());
 /* PAT WC + MTRR UC/WC/WB; partial ranges and undefined overlaps fall back. */
 assert(cache_wc_range(&cache_bsp,0x80000000,4096000));
 cache_bsp.base[0]|=1;assert(cache_wc_range(&cache_bsp,0x80000000,4096000));
 cache_bsp.base[0]=0x80000006;assert(cache_wc_range(&cache_bsp,0x80000000,4096000));
 cache_bsp.base[0]=0x80000004;assert(!cache_wc_range(&cache_bsp,0x80000000,4096000));
 cache_bsp.base[0]=0x80000005;assert(!cache_wc_range(&cache_bsp,0x80000000,4096000));
 cache_bsp.base[0]=0x80000006;cache_bsp.mask[0]=0xffffffe00800ULL;
 assert(!cache_wc_range(&cache_bsp,0x80000000,4096000));
 cache_bsp.mask[0]=0xffff80000800ULL;
 cache_bsp.base[1]=0x80000001;cache_bsp.mask[1]=cache_bsp.mask[0];
 assert(!cache_wc_range(&cache_bsp,0x80000000,4096000));
 cache_bsp.base[1]=0x80000000;assert(cache_wc_range(&cache_bsp,0x80000000,4096000));
 cache_bsp.mask[1]|=4096;assert(!cache_wc_range(&cache_bsp,0x80000000,4096000));
 puts("x86 cache policy: PAT selectors, active huge/4K aliases, AP admission, MTRRs and UC fallback passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-cache-') as tmp:
    path = Path(tmp) / 'cache.c'
    path.write_text(prefix + source + suffix)
    binary = path.with_suffix('')
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'gcc', '-std=gnu17',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}, check=True)
