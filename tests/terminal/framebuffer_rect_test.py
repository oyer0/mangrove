#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounds and pitch checks for the production framebuffer rectangle copier."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/framebuffer.c').read_text()
start = source.index('void framebuffer_flush_rows(')
body = source[start:]
# Host adapter for the x86 REP MOVSD instruction, retaining the production
# clipping, pitch arithmetic and per-row traversal on non-x86 hosts too.
body, replacements = re.subn(r'asm volatile\s*\([\s\S]*?\n\s*\);',
    lambda m: 'memcpy(dst, src, ' + ('total_pixels' if 'total_pixels' in m[0] else 'count') + ' * sizeof(u32));', body)
assert replacements == 2
fence = re.search(r'static void framebuffer_complete_writes\(void\)\n\{[^}]*\}', source)[0]
fence = fence.replace('__asm__ volatile("sfence" ::: "memory");', 'fence_count++;')

def function(name):
    match = re.search(r'(?:static )?void ' + name + r'\([^;]*?\)\s*\{', source)
    assert match
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


read_barrier = function('framebuffer_before_read').replace(
    '__asm__ volatile("mfence" ::: "memory");', 'publish_pending();')
copy_rows = function('framebuffer_copy_rows')
# Keep the forward/backward overlap arithmetic, adapting only REP MOVSD.
copy_rows, replacements = re.subn(r'asm volatile\s*\([\s\S]*?\n\s*\);',
    lambda m: ('memmove(dst - total_pixels + 1, src - total_pixels + 1, total_pixels * sizeof(u32));'
               if 'std\\n' in m[0] else 'memmove(dst, src, total_pixels * sizeof(u32));'), copy_rows)
assert replacements == 2
snapshot = function('framebuffer_enable_backbuffer')
snapshot, replacements = re.subn(r'asm volatile\s*\([\s\S]*?\n\s*\);',
    'memcpy(dst, src, total_pixels * sizeof(u32));', snapshot)
assert replacements == 1

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
typedef uint32_t u32; typedef uint64_t u64;
static u32 ram[8*6], video[8*6];
static u32 *backbuffer=ram,*framebuffer=video;
static u32 width=5,height=6,pitch=8;
static bool write_combining;
static unsigned fence_count,read_fences,redraws;
static bool pending;
static void publish_pending(void){read_fences++;if(pending){video[16]=777;pending=false;}}
static void *kmalloc(u64 bytes){assert(bytes==sizeof(ram));return ram;}
static void terminal_redraw(void){redraws++;}
'''+fence+'\n'+read_barrier+'\n'+copy_rows+'\n'+snapshot+'\n'+body+r'''
int main(void) {
 for(u32 y=0;y<6;y++)for(u32 x=0;x<8;x++)ram[y*8+x]=100+y*8+x;
 for(u32 left=0;left<8;left++)for(u32 top=0;top<8;top++) {
  const u32 sizes[]={0,1,3,8,UINT_MAX};
  for(unsigned wi=0;wi<5;wi++)for(unsigned hi=0;hi<5;hi++) {
   for(unsigned i=0;i<48;i++)video[i]=0xdeadbeef;
   framebuffer_flush_rect(left,top,sizes[wi],sizes[hi]);
   for(u32 y=0;y<6;y++)for(u32 x=0;x<8;x++) {
    bool copied=x<width && x>=left && y>=top &&
       (u64)x-left<sizes[wi] && (u64)y-top<sizes[hi];
    assert(video[y*8+x]==(copied?ram[y*8+x]:0xdeadbeef));
   }
  }
 }
 assert(fence_count==0);
 write_combining=true;framebuffer_flush_rect(0,0,5,6);assert(fence_count==1);
 framebuffer_flush_rect(0,0,0,6);assert(fence_count==1);
 framebuffer_flush_rect(9,0,5,6);assert(fence_count==1);
 for(unsigned i=0;i<48;i++)video[i]=0xdeadbeef;
 framebuffer_flush_rows(1,2);assert(fence_count==2);
 for(unsigned i=0;i<48;i++)assert(video[i]==(i>=8&&i<24?ram[i]:0xdeadbeef));
 framebuffer_flush_rows(1,0);assert(fence_count==2);
 backbuffer=0;framebuffer_flush_rows(0,1);assert(fence_count==3);
 backbuffer=0;framebuffer_flush_rect(0,0,UINT_MAX,UINT_MAX);
 assert(fence_count==4);
 backbuffer=video;framebuffer_flush_rect(0,0,UINT_MAX,UINT_MAX);
 assert(fence_count==5);
 write_combining=false;framebuffer_flush_rows(0,6);assert(fence_count==5);
 write_combining=true;backbuffer=0;video[16]=111;pending=true;
 framebuffer_copy_rows(1,2,1);assert(video[8]==777&&read_fences==1);
 video[16]=111;pending=true;framebuffer_enable_backbuffer();
 assert(ram[16]==777&&read_fences==2&&redraws==1);
 framebuffer_copy_rows(1,2,1);assert(read_fences==2);
 ram[8]=888;framebuffer_copy_rows(2,1,1);
 assert(ram[16]==888&&read_fences==2);
 return 0;
}
'''
fixture = '#include <stdbool.h>\n' + fixture
with tempfile.TemporaryDirectory(prefix='mangrove-framebuffer-rect-') as tmp:
    cfile=Path(tmp)/'rect.c'; binary=Path(tmp)/'rect'; cfile.write_text(fixture)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc', '-std=c11',
        '-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        str(cfile),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,
        env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
print('framebuffer copies: clipping, pitch padding and WC completion fences passed')
