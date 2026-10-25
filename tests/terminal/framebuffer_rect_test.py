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
start = source.index('void framebuffer_flush_rect(')
body = source[start:]
# Host adapter for the x86 REP MOVSD instruction, retaining the production
# clipping, pitch arithmetic and per-row traversal on non-x86 hosts too.
body, replacements = re.subn(r'asm volatile\s*\([\s\S]*?\n        \);',
                             'memcpy(dst, src, count * sizeof(u32));', body)
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
'''+body+r'''
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
 backbuffer=0;framebuffer_flush_rect(0,0,UINT_MAX,UINT_MAX);
 backbuffer=video;framebuffer_flush_rect(0,0,UINT_MAX,UINT_MAX);
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
print('framebuffer rectangles: clipping, zero/overflow extents and pitch padding passed')
