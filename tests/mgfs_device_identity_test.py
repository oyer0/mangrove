#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production block admission and MGFS I/O with a controlled registry device.

Pthread mutexes model kernel parking; callbacks and media are fixtures. This
does not emulate DMA, hardware removal, or whole-kernel scheduling.
"""
from pathlib import Path
import ast
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
mutex_test = ast.parse((ROOT / "tests/mutex_test.py").read_text())
pre = next(ast.literal_eval(n.value) for n in mutex_test.body
           if isinstance(n, ast.Assign) and
           any(isinstance(t, ast.Name) and t.id == "pre" for t in n.targets))


def function(source, name):
    for match in re.finditer(r"^(?:static )?[^\n;{}]*\b" + name + r"\s*\(",
                             source, re.M):
        brace = source.index("{", match.end())
        if ";" in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise RuntimeError(name)


def strip(source):
    return re.sub(r"^(?:#include .*|#pragma once)\n", "", source, flags=re.M)


mutex = strip((ROOT / "kernel/src/mutex.c").read_text())
mutex = re.sub(r"static u64 mutex_irq_save\(void\)\n\{.*?\n}",
               "static u64 mutex_irq_save(void){return 0;}", mutex,
               count=1, flags=re.S)
mutex = re.sub(r"static void mutex_irq_restore\(u64 flags\)\n\{.*?\n}",
               "static void mutex_irq_restore(u64 flags){(void)flags;}",
               mutex, count=1, flags=re.S)
wake = function((ROOT / "kernel/src/scheduler.c").read_text(),
                "scheduler_mutex_wake")
block = strip((ROOT / "kernel/src/block.c").read_text())
block = block.replace("static mutex_t block_io_gate;",
                      "static mutex_t block_io_gate={.state_lock=PTHREAD_MUTEX_INITIALIZER};")
block = block.replace("static spinlock_t block_registry_lock;",
                      "static spinlock_t block_registry_lock=PTHREAD_MUTEX_INITIALIZER;")
mgfs = (ROOT / "kernel/src/storage/mgfs.c").read_text()
vfs = (ROOT / "kernel/src/vfs.c").read_text()
header = strip((ROOT / "kernel/include/block.h").read_text())

types = "typedef uint8_t u8; typedef uint32_t u32; typedef size_t usize;\n"
fixture = r'''
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#define MG_EVENT_CLASS_DEVICE 1
#define MG_EVENT_CLASS_BLOCK 2
#define MG_EVENT_BLOCK_ADDED 3
#define MG_EVENT_BLOCK_REMOVED 4
#define MGFS_BLOCK_BYTES 4096U
#define MGFS_U64_MAX UINT64_MAX
#define MGFS_EXTENT_DATA 1ULL
static void ipc_publish_event(unsigned cls,unsigned event,u64 id,const char *name)
{(void)cls;(void)event;(void)id;(void)name;}
static void vfs_device_removed(block_device_t *device){(void)device;}
static const char *mgfs_error;
static void *kmalloc(usize n){return malloc(n);}
static void kfree(void *p){free(p);}
typedef struct {block_device_t *dev;u64 device_id;
                struct {u64 data_start,data_blocks,record_count;} layout;} mgfs_fs_t;
'''
vfs_fixture = r'''
typedef struct {mutex_t operation_lock;} vfs_super_t;
typedef struct {vfs_super_t *super;} vfs_node_t;
static atomic_bool super_live = true;
static unsigned node_releases;
static bool vfs_super_is_live(const vfs_super_t *sb){return sb && atomic_load(&super_live);}
static bool vfs_node_retain_super(vfs_node_t *node,vfs_super_t **out)
{if(!atomic_load(&super_live))return false;*out=node->super;return true;}
static void vfs_super_release(vfs_super_t *sb){(void)sb;node_releases++;}
'''
test = r'''
static u8 media[512 * 512];
static unsigned read_calls,write_calls,last_read_sectors;
static pthread_mutex_t callback_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_cond=PTHREAD_COND_INITIALIZER;
static bool hold_callback,callback_entered,callback_continue;
static bool backend_read(block_device_t *d,u64 lba,u32 count,void *out)
{
    (void)d;assert(lba+count<=512);read_calls++;last_read_sectors=count;
    pthread_mutex_lock(&callback_lock);
    if(hold_callback){callback_entered=true;pthread_cond_broadcast(&callback_cond);
        while(!callback_continue)pthread_cond_wait(&callback_cond,&callback_lock);}
    pthread_mutex_unlock(&callback_lock);
    memcpy(out,media+lba*512,(usize)count*512);return true;
}
static bool backend_write(block_device_t *d,u64 lba,u32 count,const void *in)
{(void)d;assert(lba+count<=512);write_calls++;
 memcpy(media+lba*512,in,(usize)count*512);return true;}
typedef struct {kernel_thread_t thread;block_device_t *dev;u64 id;
                bool result;u8 data[4096];} read_worker_t;
static void *read_worker(void *arg)
{read_worker_t *w=arg;self=&w->thread;
 w->result=block_read_for_id(w->dev,w->id,24,8,w->data);return NULL;}
typedef struct {kernel_thread_t thread;vfs_super_t *sb;bool result;
                bool node;} lock_worker_t;
static void *lock_worker(void *arg)
{lock_worker_t *w=arg;self=&w->thread;
 if(w->node){vfs_node_t node={w->sb};vfs_super_t *out=NULL;
    w->result=vfs_node_operation_begin(&node,&out);}
 else w->result=vfs_super_operation_lock(w->sb);
 if(w->result)mutex_unlock(&w->sb->operation_lock);
 return NULL;}
static void wait_for_gate_waiter(void)
{
    for(unsigned i=0;i<1000000;i++){
        u64 f=spin_lock_irqsave(&block_io_gate.state_lock);
        bool waiting=block_io_gate.wait_head!=NULL;
        spin_unlock_irqrestore(&block_io_gate.state_lock,f);
        if(waiting)return;
        sched_yield();
    }
    assert(!"worker did not park on block gate");
}
static void wait_for_mutex_waiter(mutex_t *m)
{
    for(unsigned i=0;i<1000000;i++){
        u64 f=spin_lock_irqsave(&m->state_lock);
        bool waiting=m->wait_head!=NULL;
        spin_unlock_irqrestore(&m->state_lock,f);
        if(waiting)return;
        sched_yield();
    }
    assert(!"worker did not park on operation lock");
}
int main(void)
{
    kernel_thread_t main_thread={.state=THREAD_STATE_RUNNING,
                                  .condition=PTHREAD_COND_INITIALIZER};
    self=&main_thread;
    block_device_t source={.sector_size=512,.sector_count=512,
                           .read=backend_read,.write=backend_write};
    assert(block_register(&source));
    block_device_t *dev=block_get_device(0);assert(dev);
    u64 old_id=dev->id;mgfs_fs_t fs={.dev=dev,.device_id=old_id,
                                     .layout={1,63,64}};
    u8 data[MGFS_BLOCK_BYTES],written[MGFS_BLOCK_BYTES];
    memset(written,0x5a,sizeof(written));
    assert(mgfs_write_block(dev,old_id,2,written));
    assert(mgfs_read_block(dev,old_id,2,data));
    assert(!memcmp(data,written,sizeof(data))&&write_calls==1&&read_calls==1);
    block_device_t copy=*dev;
    assert(!block_read_for_id(&copy,old_id,16,8,data));
    assert(!block_write_for_id(&copy,old_id,16,8,written));
    assert(read_calls==1&&write_calls==1);
    u64 extent[4]={0,16,8,MGFS_EXTENT_DATA};
    u64 logical=0,physical=0,count=0;bool found=false;
    assert(mgfs_validate_file_extent(&fs,(u8*)extent,&logical,&physical,&count,
                                     3,data,&found)&&found);
    assert(read_calls==2&&last_read_sectors==64); /* Grouped read. */

    /* A request that parked after its initial identity check must reject
       retirement when the gate becomes available. */
    assert(block_io_acquire());
    read_worker_t queued={.thread={.state=THREAD_STATE_RUNNING,
                                    .condition=PTHREAD_COND_INITIALIZER},
                          .dev=dev,.id=old_id};
    pthread_t thread;assert(!pthread_create(&thread,NULL,read_worker,&queued));
    wait_for_gate_waiter();
    unsigned prior_reads=read_calls,prior_writes=write_calls;
    assert(block_device_mark_gone(old_id));
    assert(!mgfs_read_block(dev,old_id,2,data));
    assert(!mgfs_write_block(dev,old_id,2,written));
    assert(read_calls==prior_reads&&write_calls==prior_writes);
    kernel_thread_t remover={.state=THREAD_STATE_RUNNING,
                              .condition=PTHREAD_COND_INITIALIZER};
    self=&remover;
    assert(!block_unregister_by_id(old_id)); /* An admitted gate owner drains. */
    self=&main_thread;
    block_io_release();assert(!pthread_join(thread,NULL));
    assert(!queued.result&&read_calls==prior_reads&&write_calls==prior_writes);
    assert(block_unregister_by_id(old_id));
    assert(block_register(&source));assert(block_get_device(0)==dev);
    assert(dev->id!=old_id); /* Same registry slot, new immutable ID. */
    assert(!mgfs_read_block(dev,old_id,2,data));
    assert(!mgfs_write_block(dev,old_id,2,written));
    logical=physical=count=0;found=false;
    assert(!mgfs_validate_file_extent(&fs,(u8*)extent,&logical,&physical,
                                      &count,3,data,&found));
    assert(!block_read_for_id(dev,old_id,16,8,data));
    assert(!block_write_for_id(dev,old_id,16,8,written));
    assert(read_calls==prior_reads&&write_calls==prior_writes);
    assert(mgfs_read_block(dev,dev->id,2,data));

    /* A callback admitted before retirement may finish, but unregister
       cannot reclaim its backing while the callback owns the gate. */
    hold_callback=true;callback_entered=callback_continue=false;
    read_worker_t active={.thread={.state=THREAD_STATE_RUNNING,
                                    .condition=PTHREAD_COND_INITIALIZER},
                          .dev=dev,.id=dev->id};
    assert(!pthread_create(&thread,NULL,read_worker,&active));
    pthread_mutex_lock(&callback_lock);
    while(!callback_entered)pthread_cond_wait(&callback_cond,&callback_lock);
    pthread_mutex_unlock(&callback_lock);
    assert(block_device_mark_gone(dev->id));
    assert(!block_unregister_by_id(dev->id));
    pthread_mutex_lock(&callback_lock);callback_continue=true;
    pthread_cond_broadcast(&callback_cond);pthread_mutex_unlock(&callback_lock);
    assert(!pthread_join(thread,NULL));assert(!active.result);
    hold_callback=false;assert(block_unregister_by_id(dev->id));

    /* Production VFS lock entry rejects a caller that wakes after loss. */
    vfs_super_t sb;mutex_init(&sb.operation_lock);
    assert(mutex_lock(&sb.operation_lock));atomic_store(&super_live,true);
    lock_worker_t blocked={.thread={.state=THREAD_STATE_RUNNING,
                                     .condition=PTHREAD_COND_INITIALIZER},
                           .sb=&sb,.node=false};
    assert(!pthread_create(&thread,NULL,lock_worker,&blocked));
    wait_for_mutex_waiter(&sb.operation_lock);atomic_store(&super_live,false);
    assert(mutex_unlock(&sb.operation_lock));assert(!pthread_join(thread,NULL));
    assert(!blocked.result);
    assert(mutex_lock(&sb.operation_lock));atomic_store(&super_live,true);
    blocked=(lock_worker_t){.thread={.state=THREAD_STATE_RUNNING,
                                    .condition=PTHREAD_COND_INITIALIZER},
                            .sb=&sb,.node=true};
    assert(!pthread_create(&thread,NULL,lock_worker,&blocked));
    wait_for_mutex_waiter(&sb.operation_lock);atomic_store(&super_live,false);
    assert(mutex_unlock(&sb.operation_lock));assert(!pthread_join(thread,NULL));
    assert(!blocked.result&&node_releases==1);
    puts("MGFS identity: bound admission, slot reuse, removal races, grouped reads and VFS wake checks passed");
}
'''

source = (pre + types + header + fixture + wake + mutex + block +
          "\n".join(function(mgfs, name) for name in
                    ("mgfs_get_le64", "mgfs_set_error", "mgfs_read_block",
                     "mgfs_write_block", "mgfs_validate_file_extent")) +
          vfs_fixture + function(vfs, "vfs_super_operation_lock") +
          function(vfs, "vfs_node_operation_begin") + test)
with tempfile.TemporaryDirectory(prefix="mangrove-mgfs-identity-") as directory:
    path = Path(directory) / "test.c"
    exe = Path(directory) / "test"
    path.write_text(source)
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-fsanitize=address,undefined", str(path), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
