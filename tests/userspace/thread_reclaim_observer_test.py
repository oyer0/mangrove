#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native observer rejects stable-but-unreclaimed and below-baseline samples."""
import os
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/shared'))
from host_fixture import HostFixture
CC=sys.argv[1] if len(sys.argv)>1 else 'gcc'
s=(ROOT/'tests/userspace/thread_lifecycle_guest.c').read_text()
a=s.index('static bool own_process_memory_wait_for(');b=s.index('{',a);e=b+1;d=1
while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;typedef uint32_t u32;
#define MG_PROCESS_FLAG_REAP_STATE_MASK 7U
static unsigned at,count;static u64 now;static u64 values[8];static u32 flags[8];
static bool own_process_memory_bytes(u64*out,u32*f){
 unsigned i=at<count?at:count-1;*out=values[i];*f=flags[i];at++;return true;}
static u64 uptime_ms(void){return now;}
static void process_yield(void){now++;}
'''+s[a:e]+r'''
int main(void){u64 last;u32 f,samples;
 values[0]=values[1]=136;values[2]=100;count=3;
 assert(own_process_memory_wait_for(100,8,&last,&f,&samples)&&samples==3&&last==100);
 at=now=0;count=1;values[0]=136;
 assert(!own_process_memory_wait_for(100,8,&last,&f,&samples)&&last==136);
 /* The failure seen after the IPC probe: an inflated 163840 oracle must
  * never be accepted just because the observed 126976 is below it. */
 at=now=0;values[0]=126976;
 assert(!own_process_memory_wait_for(163840,8,&last,&f,&samples)&&last==126976);
 at=now=0;values[0]=100;count=3;values[1]=values[2]=100;
 flags[0]=flags[1]=1;flags[2]=0;
 assert(own_process_memory_wait_for(100,8,&last,&f,&samples)&&samples==3&&!f);
 puts("native reclaim observer: exact pre-worker baseline and cleared flags; repeated/undershooting samples never pass PASS");}
'''
# The oracle must precede this process's first native worker, including the IPC
# probe. Child-process helpers use separate address spaces.
main=s[s.index('int main(int argc, char **argv)'):]
assert main.index('own_process_memory_stable(')<main.index('verify_native_ipc(')<main.index('mg_thread_create(')
with HostFixture('thread-reclaim-observer') as fixture:
    source=fixture.path/'observer.c';source.write_text(code);binary=fixture.path/'observer'
    fixture.run([CC,'-std=gnu17','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined',str(source),'-o',str(binary)])
    print(fixture.run([str(binary)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}).stdout,end='')
