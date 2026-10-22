#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify production process writes serialize with alternate-screen ownership."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/terminal.c').read_text()
object_source = (ROOT / 'kernel/src/object.c').read_text()
assert 'terminal_write_for_process(process->pid' in object_source
assert 'terminal_process_output_allowed(process->pid)' not in object_source


def function(name):
    match = next(m for m in re.finditer(
        r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(', source, re.M)
        if ';' not in source[m.end():source.index('{', m.end())])
    brace = source.index('{', match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


pre = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <types.h>
#include <spinlock.h>
#include <mangrove_errors.h>

typedef struct {bool alternate_active;u64 alternate_owner_pid;} terminal_state_t;
static terminal_state_t terminal;
static spinlock_t terminal_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_condition=PTHREAD_COND_INITIALIZER;
static bool pause_render,render_entered,allow_render;
static _Atomic bool owner_change_lock_attempted,owner_change_done;
static _Thread_local bool in_owner_change;
static _Thread_local u64 writer_pid;
static char rendered[64];static u64 rendered_pid[64];static unsigned rendered_count;
static bool terminal_process_controls_locked(u64 process_id);

void spinlock_init(spinlock_t*l){assert(!pthread_mutex_init(l,NULL));}
u64 spin_lock_irqsave(spinlock_t*l){
 if(in_owner_change&&l==&terminal_lock)
  atomic_store_explicit(&owner_change_lock_attempted,true,memory_order_release);
 assert(!pthread_mutex_lock(l));return 0;
}
void spin_unlock_irqrestore(spinlock_t*l,u64 f){(void)f;assert(!pthread_mutex_unlock(l));}
static void terminal_overlay_clear_locked(bool redraw){(void)redraw;}
static void terminal_begin_batch_locked(void){}
static void terminal_end_batch_locked(bool flush){(void)flush;}
static void terminal_putc_locked(char c){
 assert(terminal_process_controls_locked(writer_pid));
 if(pause_render&&!render_entered){
  assert(!pthread_mutex_lock(&gate_lock));render_entered=true;
  assert(!pthread_cond_broadcast(&gate_condition));
  while(!allow_render)assert(!pthread_cond_wait(&gate_condition,&gate_lock));
  assert(!pthread_mutex_unlock(&gate_lock));
 }
 assert(rendered_count<sizeof(rendered));
 rendered[rendered_count]=c;rendered_pid[rendered_count]=writer_pid;rendered_count++;
}
'''

post = r'''
typedef struct {u64 pid;i64 result;} writer_t;
static void *writer(void *p){writer_t*w=p;writer_pid=w->pid;w->result=terminal_write_for_process(w->pid,"abc",3);return NULL;}
static void *acquire_alternate(void *unused){
 (void)unused;in_owner_change=true;
 u64 f=spin_lock_irqsave(&terminal_lock);
 terminal.alternate_active=true;terminal.alternate_owner_pid=2;
 spin_unlock_irqrestore(&terminal_lock,f);
 atomic_store_explicit(&owner_change_done,true,memory_order_release);return NULL;
}
int main(void){
 writer_t normal={.pid=1};pthread_t writer_thread,owner_thread;
 pause_render=true;assert(!pthread_create(&writer_thread,NULL,writer,&normal));
 assert(!pthread_mutex_lock(&gate_lock));
 while(!render_entered)assert(!pthread_cond_wait(&gate_condition,&gate_lock));
 assert(!pthread_mutex_unlock(&gate_lock));
 assert(!pthread_create(&owner_thread,NULL,acquire_alternate,NULL));
 while(!atomic_load_explicit(&owner_change_lock_attempted,memory_order_acquire))
  sched_yield();
 assert(!atomic_load_explicit(&owner_change_done,memory_order_acquire));
 /* Ownership cannot change mid-write after the transition has actually
  * attempted to acquire terminal_lock. */
 assert(!pthread_mutex_lock(&gate_lock));allow_render=true;
 assert(!pthread_cond_broadcast(&gate_condition));assert(!pthread_mutex_unlock(&gate_lock));
 assert(!pthread_join(writer_thread,NULL));assert(!pthread_join(owner_thread,NULL));
 assert(normal.result==3&&rendered_count==3);
 for(unsigned i=0;i<3;i++)assert(rendered_pid[i]==1);

 /* Once the alternate owner has acquired the lock, the former normal writer
  * is rejected and cannot modify its screen. The current owner can write. */
 u64 before=rendered_count;writer_pid=1;
 assert(terminal_write_for_process(1,"x",1)==MG_ERR_ACCESS_DENIED);
 assert(rendered_count==before);
 writer_pid=2;assert(terminal_write_for_process(2,"O",1)==1);
 assert(rendered_count==before+1&&rendered[before]=='O');

 /* Release returns authorization to ordinary processes under that same lock. */
 u64 f=spin_lock_irqsave(&terminal_lock);terminal.alternate_active=false;
 terminal.alternate_owner_pid=0;spin_unlock_irqrestore(&terminal_lock,f);
 writer_pid=1;assert(terminal_write_for_process(1,"N",1)==1);
 assert(rendered[rendered_count-1]=='N');
 assert(terminal_write_for_process(0,"x",1)==MG_ERR_BAD_ARGUMENT);
 assert(terminal_write_for_process(1,NULL,1)==MG_ERR_BAD_ARGUMENT);
 puts("terminal output: permission and rendering are serialized with alternate ownership");
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-terminal-owner-') as directory:
    temp = Path(directory)
    (temp / 'spinlock.h').write_text('#pragma once\n#include <pthread.h>\n#include <types.h>\n'
                                     'typedef pthread_mutex_t spinlock_t;\n'
                                     'void spinlock_init(spinlock_t*);\n'
                                     'u64 spin_lock_irqsave(spinlock_t*);\n'
                                     'void spin_unlock_irqrestore(spinlock_t*,u64);\n')
    write = function('terminal_write_for_process')
    is_owner = function('terminal_process_is_owner_locked')
    controls = function('terminal_process_controls_locked')
    # The writer test's assertion inside rendering uses the production helper.
    source_text = pre + is_owner + controls + write + post
    cfile = temp / 'test.c';cfile.write_text(source_text)
    binary = temp / 'test'
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc','-std=gnu17',
                    '-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-I',str(temp),
                    '-I',str(ROOT/'include'),'-idirafter',str(ROOT/'libc/include'),
                    str(cfile),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=30,
                   env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
