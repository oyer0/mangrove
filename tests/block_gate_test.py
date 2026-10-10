#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production recursive block gate and mutex with pthread scheduler fixture.
Verifies callback draining/admission and contention, not hardware quiescence.
"""
from pathlib import Path
import ast
import re
import subprocess
import sys
import tempfile
ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent
# Reuse the explicitly mocked scheduler/IRQ fixture, without executing its test.
module = ast.parse((HERE / 'mutex_test.py').read_text())
pre = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'pre' for t in n.targets))
pre = pre.replace('static spinlock_t scheduler_lock=', 'static spinlock_t scheduler_lock=')
mutex = (ROOT / 'kernel/src/mutex.c').read_text()
mutex = re.sub(r'^#include .*\n', '', mutex, flags=re.M)
mutex = re.sub(r'static u64 mutex_irq_save\(void\).*?\n}', 'static u64 mutex_irq_save(void){return 0;}', mutex, count=1, flags=re.S)
mutex = re.sub(r'static void mutex_irq_restore\(u64 flags\).*?\n}', 'static void mutex_irq_restore(u64 flags){(void)flags;}', mutex, count=1, flags=re.S)
sched = (ROOT / 'kernel/src/scheduler.c').read_text()
m = re.search(r'^bool scheduler_mutex_wake\(.*?\n}', sched, re.M | re.S)
wake = m.group(0) if m else ''
block = (ROOT / 'kernel/src/block.c').read_text()
start = block.index('static mutex_t block_io_gate;') if 'static mutex_t block_io_gate;' in block else block.index('#define BLOCK_IO_WAITERS')
gate = block[start:block.index('static bool block_cache_range_overlaps')]
gate = re.sub(r'static u64 block_io_irq_save\(void\)\n\{.*?\n}', 'static u64 block_io_irq_save(void){return 0;}', gate, count=1, flags=re.S)
gate = re.sub(r'static void block_io_irq_restore\(u64 flags\)\n\{.*?\n}', 'static void block_io_irq_restore(u64 flags){(void)flags;}', gate, count=1, flags=re.S)
if 'static void block_callback_enter' not in gate:
    gate += 'static void block_callback_enter(void){}\nstatic void block_callback_leave(void){}\n'
if 'static mutex_t block_io_gate;' in gate:
    gate = gate.replace('static mutex_t block_io_gate;', 'static mutex_t block_io_gate = {.state_lock=PTHREAD_MUTEX_INITIALIZER};')
post = r'''
static unsigned count;
static void *worker(void *arg){self=arg;
 for(unsigned n=0;n<10000;n++){
  assert(block_io_acquire());assert(block_io_acquire());
  block_callback_enter();assert(!block_io_begin_quiesce());
  count++;block_callback_leave();block_io_release();block_io_release();
 }
 return NULL;
}
int main(void){
 kernel_thread_t a={.state=THREAD_STATE_RUNNING,.condition=PTHREAD_COND_INITIALIZER};
 kernel_thread_t b={.state=THREAD_STATE_RUNNING,.condition=PTHREAD_COND_INITIALIZER};
 self=&a;assert(block_io_acquire());block_callback_enter();
 /* Same owner is not drained: the old implementation incorrectly admits it. */
 assert(!block_io_begin_quiesce());
 self=&b;assert(!block_io_begin_quiesce());
 self=&a;assert(block_io_acquire());block_callback_enter();
 assert(!block_io_begin_quiesce());block_callback_leave();block_io_release();
 assert(!block_io_begin_quiesce());block_callback_leave();
 assert(block_io_begin_quiesce());block_io_end_quiesce();block_io_release();
 self=&b;assert(block_io_begin_quiesce());assert(block_io_begin_quiesce());
 self=&a;assert(!block_io_begin_quiesce());
 self=&b;block_io_end_quiesce();block_io_end_quiesce();block_io_end_quiesce();
 /* Waiting/cancelled owner ordering is tested in the production mutex suite. */
 kernel_thread_t ts[4]={0};pthread_t ps[4];
 for(unsigned i=0;i<4;i++){ts[i].state=THREAD_STATE_RUNNING;pthread_cond_init(&ts[i].condition,NULL);assert(!pthread_create(&ps[i],NULL,worker,&ts[i]));}
 for(unsigned i=0;i<4;i++)assert(!pthread_join(ps[i],NULL));
 assert(count==40000);self=&a;assert(block_io_begin_quiesce());block_io_end_quiesce();
 puts("block gate: production recursive ownership, callback drain and 40,000 concurrent operations passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-block-gate-') as d:
    c = Path(d) / 'test.c'
    c.write_text(pre + '\ntypedef unsigned u32;\n#define BLOCK_MAX_DEVICES 32\n' + wake + '\n' + mutex + gate + post)
    exe = Path(d) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
