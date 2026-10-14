#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production block registry/gate, GPT retirement and mutation admission.
Uses the mutex suite's pthread park/IRQ fixture; no hardware is emulated here.
"""
from pathlib import Path
import ast
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
module = ast.parse((ROOT / 'tests/mutex_test.py').read_text())
pre = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'pre' for t in n.targets))

def function(source, name):
    for m in re.finditer(r'^(?:static )?[^\n;{}]*\b' + name + r'\s*\(', source, re.M):
        b = source.index('{', m.end())
        if ';' in source[m.end():b]:
            continue
        end, depth = b + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[m.start():end] + '\n'
    raise ValueError(name)

def strip(s):
    return re.sub(r'^(?:#include .*|#pragma once)\n', '', s, flags=re.M)
mutex = strip((ROOT / 'kernel/src/mutex.c').read_text())
mutex = re.sub(r'static u64 mutex_irq_save\(void\)\n\{.*?\n}', 'static u64 mutex_irq_save(void){return 0;}', mutex, count=1, flags=re.S)
mutex = re.sub(r'static void mutex_irq_restore\(u64 flags\)\n\{.*?\n}', 'static void mutex_irq_restore(u64 flags){(void)flags;}', mutex, count=1, flags=re.S)
block = strip((ROOT / 'kernel/src/block.c').read_text())
block = block.replace('static mutex_t block_io_gate;', 'static mutex_t block_io_gate={.state_lock=PTHREAD_MUTEX_INITIALIZER};').replace('static spinlock_t block_registry_lock;', 'static spinlock_t block_registry_lock=PTHREAD_MUTEX_INITIALIZER;')
gpt = (ROOT / 'kernel/src/storage/gpt.c').read_text()
fmt = (ROOT / 'kernel/src/storage/format.c').read_text()
headers = strip((ROOT / 'kernel/include/block.h').read_text()) + strip((ROOT / 'kernel/include/storage/gpt.h').read_text())
model = r'''
#include <string.h>
typedef unsigned u32;typedef unsigned char u8;typedef size_t usize;
#define MG_EVENT_CLASS_DEVICE 1
#define MG_EVENT_CLASS_BLOCK 2
#define MG_EVENT_BLOCK_ADDED 3
#define MG_EVENT_BLOCK_REMOVED 4
#define GPT_REGISTERED_MAX 16
static bool notify_race, remove_during_callback;
static kernel_thread_t remover={.state=THREAD_STATE_RUNNING,.condition=PTHREAD_COND_INITIALIZER};
static void ipc_publish_event(unsigned cls,unsigned event,u64 id,const char *name){(void)cls;(void)event;(void)id;(void)name;}
static void vfs_device_removed(block_device_t *device){
 if(notify_race){notify_race=false;assert(!block_unregister_by_id(device->id));assert(device->registered);}
}
static gpt_partition_info_t partitions[16];static u32 partition_count;
static bool partition_registered[16];static u64 partition_ids[16];
static spinlock_t partition_state_lock=PTHREAD_MUTEX_INITIALIZER;
static bool storage_operation_in_progress;
'''
# model needs block prototypes/types before VFS stub; split type preamble.
types, model = model.split('static bool notify_race', 1)
model = 'static bool notify_race' + model
post = r'''
static block_device_t source;
static bool backend_read(block_device_t*d,u64 l,u32 n,void*b){(void)l;(void)n;memset(b,0,512);
 if(remove_during_callback){
  kernel_thread_t *saved=self;self=&remover;
  assert(!block_unregister_by_id(d->id));
  assert(d->registered && !d->online && d->driver_data);
  assert(!block_read(d,0,1,b)); /* retirement rejects before parking */
  self=saved;assert(!block_io_begin_quiesce());
 }
 return true;
}
static unsigned mutation_count;
static void *mutator(void *arg){self=arg;
 for(unsigned n=0;n<10000;n++){
  /* Busy is an explicit caller result; retries/yields here are test-only. */
  while(!storage_mutation_begin())sched_yield();
  mutation_count++;assert(storage_mutation_busy());
  storage_mutation_end();
 }
 return NULL;
}
int main(void){
 kernel_thread_t a={.state=THREAD_STATE_RUNNING,.condition=PTHREAD_COND_INITIALIZER};self=&a;
 source=(block_device_t){.sector_size=512,.sector_count=100,.read=backend_read,.driver_data=&source};
 assert(block_register(&source));block_device_t *d=block_get_device(0);u64 id=d->id;u8 data[512];
 remove_during_callback=true;assert(!block_read(d,0,1,data));
 assert(d->registered && !d->online && d->driver_data==&source);
 remove_during_callback=false;assert(block_unregister_by_id(id));assert(!d->registered);
 /* A concurrent notifier pins the registry slot until VFS has consumed ID. */
 assert(block_register(&source));d=block_get_device(0);notify_race=true;
 assert(block_unregister_by_id(d->id));assert(!d->registered);
 /* Failed unregister must retain partition backing, including same-thread
    active recursion. A later quiescent attempt removes it exactly once. */
 assert(block_register(&source));
 partitions[0]=(gpt_partition_info_t){.parent=&source,.parent_id=source.id,.first_lba=1,.last_lba=10};partition_count=1;
 block_device_t child={.type=BLOCK_DEVICE_PARTITION,.sector_size=512,.sector_count=10,.driver_data=&partitions[0],.read=backend_read};
 assert(block_register(&child));partition_ids[0]=child.id;partition_registered[0]=true;
 assert(block_io_acquire());block_callback_enter();self=&remover;
 assert(!gpt_remove_partitions_for_parent(&source));
 assert(partitions[0].parent==&source && partition_registered[0]);
 self=&a;block_callback_leave();block_io_release();
 assert(gpt_remove_partitions_for_parent(&source));assert(!partitions[0].parent && !partition_registered[0]);
 assert(gpt_remove_partitions_for_parent(&source));
 /* Cleanup must likewise preserve backing on failed unregister. */
 partitions[0]=(gpt_partition_info_t){.parent=&source,.parent_id=source.id,.first_lba=1,.last_lba=10};
 assert(block_register(&child));partition_ids[0]=child.id;partition_registered[0]=true;
 assert(block_io_acquire());block_callback_enter();u32 index=0;
 assert(!discard_allocated_partitions(&index,1));assert(partitions[0].parent);
 block_callback_leave();block_io_release();assert(discard_allocated_partitions(&index,1));
 /* Gate refusal rolls back the mutation guard rather than wedging it. */
 assert(block_io_acquire());self=&remover;
 assert(!storage_mutation_begin());assert(!storage_mutation_busy());
 self=&a;block_io_release();assert(storage_mutation_begin());
 self=&remover;assert(!storage_mutation_begin());assert(!block_io_begin_quiesce());
 self=&a;storage_mutation_end();assert(!storage_mutation_busy());
 kernel_thread_t ts[4]={0};pthread_t ps[4];
 for(unsigned i=0;i<4;i++){ts[i].state=THREAD_STATE_RUNNING;pthread_cond_init(&ts[i].condition,NULL);assert(!pthread_create(&ps[i],NULL,mutator,&ts[i]));}
 for(unsigned i=0;i<4;i++)assert(!pthread_join(ps[i],NULL));
 assert(mutation_count==40000 && !storage_mutation_busy());
 assert(block_unregister_by_id(source.id));
 puts("storage lifecycle: active callbacks, retirement, notifier pinning, failed unregister cleanup, repeated removal and 40,000 concurrent mutation admissions passed");
}
'''
wake = function((ROOT / 'kernel/src/scheduler.c').read_text(), 'scheduler_mutex_wake')
source = pre + types + headers + model + wake + mutex + block
source += ''.join(function(gpt, n) for n in ('gpt_bind_partition_parent', 'gpt_get_partition_info', 'gpt_partition_parent_matches', 'gpt_mark_partitions_gone_for_parent', 'gpt_remove_partitions_for_parent', 'discard_allocated_partitions'))
source += ''.join(function(fmt, n) for n in ('storage_mutation_begin', 'storage_mutation_end', 'storage_mutation_busy')) + post
with tempfile.TemporaryDirectory(prefix='mangrove-lifecycle-') as d:
    c = Path(d) / 'test.c'; c.write_text(source); exe = Path(d) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
