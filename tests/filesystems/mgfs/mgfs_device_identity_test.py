#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production block admission and mounted filesystem I/O with a controlled device.

Pthread mutexes model kernel parking; callbacks and media are fixtures. This
does not emulate DMA, hardware removal, or whole-kernel scheduling.
"""
from pathlib import Path
import ast
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
mutex_test = ast.parse((ROOT / "tests/kernel/mutex_test.py").read_text())
pre = next(ast.literal_eval(n.value) for n in mutex_test.body
           if isinstance(n, ast.Assign) and
           any(isinstance(t, ast.Name) and t.id == "pre" for t in n.targets))


def function(source, name):
    for match in re.finditer(r"^(?:static )?[^\n;{}]*\b" + name + r"\s*\(",
                             source, re.M):
        if match.group(0).lstrip().startswith(("if ", "else ", "return ")):
            continue
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
fat32 = (ROOT / "kernel/src/storage/fat32.c").read_text()
fat32_view = re.search(r"typedef struct \{[^}]+\} fat32_read_view_t;", fat32).group()
exfat = (ROOT / "kernel/src/storage/exfat.c").read_text()
vfs = (ROOT / "kernel/src/vfs.c").read_text()
header = strip((ROOT / "kernel/include/block.h").read_text())
header += strip((ROOT / "kernel/include/storage/gpt.h").read_text())

types = "typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef size_t usize;\n"
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
/* No recovery owners in this identity/transport fixture; full registry-owner
 * lifetime coverage is in mgfs_label_test.py. */
static bool vfs_registration_retained(u64 id){(void)id;return false;}
/* This fixture has physical devices only; real GPT aliases are exercised by
 * storage_coherency_test.py. Keep discovery dependencies explicit here. */
bool gpt_get_partition_info(block_device_t *d,gpt_partition_info_t *out)
{(void)d;(void)out;return false;}
void gpt_bind_partition_parent(block_device_t *d){(void)d;}
static const char *mgfs_error;
static void *kmalloc(usize n){return malloc(n);}
static void kfree(void *p){free(p);}
typedef struct {block_device_t *dev;u64 device_id;
                struct {u64 data_start,data_blocks,record_count;} layout;} mgfs_fs_t;
typedef struct {u64 device_id;u32 bytes_per_sector,fat_start_lba,fat_count,
                fat_size_sectors,total_clusters;bool version_confident;
                struct {u16 ext_flags;} bpb;bool ownership_proven;void *replace_transaction;} fat32_fs_t;
#define FAT32_EOC_MIN 0x0ffffff8U
#define FAT32_CLUSTER_ERR 0x0fffffffU
#define EXFAT_SECTOR_BYTES 512U
#define EXFAT_MAIN_BOOT_LBA 0ULL
#define EXFAT_BACKUP_BOOT_LBA 12ULL
typedef struct {u16 volume_flags;} exfat_boot_info_t;
typedef struct {block_device_t *dev;u64 device_id;exfat_boot_info_t boot;
                bool read_only,version_confident;
                void *replace_transaction;} exfat_fs_t;
#define VFS_OK 0
#define VFS_ERR_IO (-3)
#define VFS_ERR_DEVICE_GONE (-11)
#define VFS_ERR_UNCERTAIN (-14)
'''
vfs_fixture = r'''
typedef struct {mutex_t operation_lock;block_device_t *dev;u64 device_id;
                void *private_data;bool read_only;bool serialize_handoff;mutex_t operation_guard;void *operation_guard_owner;} vfs_super_t;
typedef struct {vfs_super_t *super;} vfs_node_t;
static atomic_bool super_live = true;
static unsigned node_releases;
static bool vfs_super_is_live(const vfs_super_t *sb){return sb && atomic_load(&super_live);}
static bool vfs_node_retain_super(vfs_node_t *node,vfs_super_t **out)
{if(!atomic_load(&super_live))return false;*out=node->super;return true;}
static void vfs_super_release(vfs_super_t *sb){(void)sb;node_releases++;}
/* Non-opt-in lock/removal test. The complete MGFS guard and its reference
 * lifetime are exercised in mgfs_handoff_test.py. */
static bool vfs_super_retain(vfs_super_t *sb){(void)sb;assert(false);return false;}
static bool vfs_super_operation_guard_release(vfs_super_t *sb){assert(!sb->serialize_handoff);return true;}
static int vfs_super_operation_error(vfs_super_t *sb){return vfs_super_is_live(sb)?VFS_OK:VFS_ERR_DEVICE_GONE;}
'''
test = r'''
static u8 media[512 * 512];
static unsigned read_calls,write_calls,flush_calls,last_read_sectors;
static bool fail_read,partial_read,fail_write,partial_write,fail_flush;
static unsigned fail_write_at;
static pthread_mutex_t callback_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_cond=PTHREAD_COND_INITIALIZER;
static bool hold_callback,callback_entered,callback_continue;
static bool backend_read(block_device_t *d,u64 lba,u32 count,void *out)
{
    (void)d;assert(lba+count<=512);read_calls++;last_read_sectors=count;
    if(fail_read)return false;
    if(partial_read){memcpy(out,media+lba*512,256);return false;}
    pthread_mutex_lock(&callback_lock);
    if(hold_callback){callback_entered=true;pthread_cond_broadcast(&callback_cond);
        while(!callback_continue)pthread_cond_wait(&callback_cond,&callback_lock);}
    pthread_mutex_unlock(&callback_lock);
    memcpy(out,media+lba*512,(usize)count*512);return true;
}
static bool backend_write(block_device_t *d,u64 lba,u32 count,const void *in)
{(void)d;assert(lba+count<=512);write_calls++;
 if(partial_write){memcpy(media+lba*512,in,512);return false;}
 if(fail_write || (fail_write_at && write_calls==fail_write_at))return false;
 memcpy(media+lba*512,in,(usize)count*512);return true;}
static bool backend_flush(block_device_t *d)
{(void)d;flush_calls++;
 pthread_mutex_lock(&callback_lock);
 if(hold_callback){callback_entered=true;pthread_cond_broadcast(&callback_cond);
     while(!callback_continue)pthread_cond_wait(&callback_cond,&callback_lock);}
 pthread_mutex_unlock(&callback_lock);
 return !fail_flush;}
static void run_mounted_filesystem_checks(block_device_t *dev,u64 id,u64 stale_id)
{
    vfs_super_t sb={.dev=dev,.device_id=id};
    fat32_fs_t fat={.device_id=id,.version_confident=true,.bytes_per_sector=512,.fat_start_lba=10,
                    .fat_count=2,.fat_size_sectors=4,.total_clusters=64};
    exfat_fs_t ex={.dev=dev,.device_id=id,.read_only=false,
                   .version_confident=true};
    sb.private_data=&fat;
    u8 in[512],out[512],before[512];
    memset(in,0x63,sizeof(in));
    fat.ownership_proven=true;
    sb.read_only=true;
    unsigned before_calls=write_calls;
    assert(!fat32_mounted_write(&sb,40,1,in) && fat.ownership_proven && write_calls==before_calls);
    assert(fat32_mounted_read(&sb,40,1,out));
    ex.read_only=true;
    assert(!exfat_write(&ex,40,1,in) && ex.version_confident && write_calls==before_calls);
    ex.read_only=false; sb.read_only=false;
    assert(fat32_mounted_write(&sb,40,1,in) && !fat.ownership_proven);
    memset(out,0,sizeof(out));assert(fat32_mounted_read(&sb,40,1,out));
    assert(!memcmp(out,in,sizeof(in)));
    assert(exfat_write(&ex,41,1,in));
    memset(out,0,sizeof(out));assert(exfat_read(&ex,41,1,out));
    assert(!memcmp(out,in,sizeof(in)));
    /* Inspect independently supplied FAT bytes without raw allocation writes.
       Pinned transaction fixtures cover mirrored mutation and recovery. */
    *(u32 *)(media+10*512+2*4)=FAT32_EOC_MIN;
    assert(fat32_get_next_cluster(&sb,&fat,2)==FAT32_EOC_MIN);
    sb.private_data=&ex;
    assert(exfat_sync(&sb)==VFS_OK);
    ex.replace_transaction=&ex;
    assert(exfat_sync(&sb)==VFS_ERR_UNCERTAIN);
    ex.replace_transaction=NULL;
    sb.private_data=&fat;
    assert(fat32_sync(&sb)==VFS_OK);
    assert(flush_calls==2);
    block_flush_t flush=dev->flush;
    dev->flush=NULL;
    assert(block_flush_for_id(dev,id)); /* Synchronous backend. */
    dev->flush=flush;
    memcpy(before,media+40*512,512);
    unsigned reads=read_calls,writes=write_calls,flushes=flush_calls;
    fat.device_id=stale_id;ex.device_id=stale_id;
    assert(fat32_sync(&sb)==VFS_ERR_DEVICE_GONE);
    assert(!fat32_mounted_read(&sb,40,1,out));
    assert(!fat32_mounted_write(&sb,40,1,in));
    assert(fat32_get_next_cluster(&sb,&fat,2)==FAT32_CLUSTER_ERR);
    assert(fat32_sync(&sb)==VFS_ERR_UNCERTAIN);
    assert(!exfat_read(&ex,41,1,out));
    assert(!exfat_write(&ex,41,1,in));
    sb.private_data=&ex;
    assert(exfat_sync(&sb)==VFS_ERR_UNCERTAIN);
    sb.private_data=&fat;
    assert(!block_flush_for_id(dev,stale_id));
    assert(read_calls==reads&&write_calls==writes&&flush_calls==flushes);
    assert(!memcmp(before,media+40*512,512));
    fat.device_id=id;ex.device_id=id;
    fat.version_confident=true; /* Independent fresh-mount scenario. */
    assert(!ex.version_confident);
    ex.version_confident=true; /* A separate fresh-mount scenario below. */
    fail_read=true;assert(!fat32_mounted_read(&sb,50,1,out));
    assert(!exfat_read(&ex,51,1,out));fail_read=false;
    partial_read=true;assert(!fat32_mounted_read(&sb,52,1,out));
    assert(!exfat_read(&ex,53,1,out));partial_read=false;
    fail_write=true;assert(!fat32_mounted_write(&sb,40,1,in));
    assert(!exfat_write(&ex,41,1,in));fail_write=false;
    assert(!ex.version_confident);
    ex.version_confident=true; /* Independent flush-failure scenario. */
    sb.private_data=&ex;
    fail_flush=true;assert(exfat_sync(&sb)==VFS_ERR_IO);fail_flush=false;
    sb.private_data=&fat;
    fat.version_confident=true; /* Independent flush-failure scenario. */
    fail_flush=true;assert(fat32_sync(&sb)==VFS_ERR_IO);fail_flush=false;
    assert(!fat.version_confident && !fat.ownership_proven);
    /* A false write can still have changed media; the identity layer
       intentionally makes no rollback or definite-failure claim. */
    memset(in,0xa4,sizeof(in));partial_write=true;
    assert(!fat32_mounted_write(&sb,42,1,in));
    assert(!exfat_write(&ex,41,1,in));partial_write=false;
    assert(!memcmp(media+42*512,in,512));
    assert(!memcmp(media+41*512,in,512));
}
typedef struct {kernel_thread_t thread;block_device_t *dev;u64 id;
                vfs_super_t *sb;exfat_fs_t *ex;unsigned kind;
                bool result;u8 data[4096];} read_worker_t;
static void *read_worker(void *arg)
{read_worker_t *w=arg;self=&w->thread;
 if(w->kind==1)w->result=fat32_mounted_read(w->sb,24,1,w->data);
 else if(w->kind==2)w->result=exfat_read(w->ex,24,1,w->data);
 else if(w->kind==3)w->result=block_flush_for_id(w->dev,w->id);
 else w->result=block_read_for_id(w->dev,w->id,24,8,w->data);
 return NULL;}
typedef struct {kernel_thread_t thread;vfs_super_t *sb;bool result;
                bool node;} lock_worker_t;
static void *lock_worker(void *arg)
{lock_worker_t *w=arg;self=&w->thread;
 if(w->node){vfs_node_t node={w->sb};vfs_super_t *out=NULL;
    w->result=vfs_node_operation_begin(&node,&out);}
 else w->result=vfs_super_operation_lock(w->sb);
 if(w->result)mutex_unlock(&w->sb->operation_lock);
 return NULL;}
static void wait_for_gate_waiters(unsigned expected)
{
    for(unsigned i=0;i<1000000;i++){
        u64 f=spin_lock_irqsave(&block_io_gate.state_lock);
        unsigned waiting=0;
        for(kernel_thread_t *w=block_io_gate.wait_head;w;w=w->mutex_wait_next)
            waiting++;
        spin_unlock_irqrestore(&block_io_gate.state_lock,f);
        if(waiting>=expected)return;
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
                           .read=backend_read,.write=backend_write,
                           .flush=backend_flush};
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
    assert(!block_flush_for_id(&copy,old_id));
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
    fat32_fs_t removed_fat={.device_id=old_id};
    vfs_super_t removed_sb={.dev=dev,.device_id=old_id,
                            .private_data=&removed_fat};
    exfat_fs_t removed_ex={.dev=dev,.device_id=old_id};
    read_worker_t queued_fs[3]={0};
    pthread_t thread,fs_threads[3];
    assert(!pthread_create(&thread,NULL,read_worker,&queued));
    for(unsigned i=0;i<3;i++){
        queued_fs[i].thread.state=THREAD_STATE_RUNNING;
        pthread_cond_init(&queued_fs[i].thread.condition,NULL);
        queued_fs[i].dev=dev;queued_fs[i].id=old_id;
        queued_fs[i].sb=&removed_sb;queued_fs[i].ex=&removed_ex;
        queued_fs[i].kind=i+1;
        assert(!pthread_create(&fs_threads[i],NULL,read_worker,&queued_fs[i]));
    }
    wait_for_gate_waiters(4);
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
    for(unsigned i=0;i<3;i++){
        assert(!pthread_join(fs_threads[i],NULL));
        assert(!queued_fs[i].result);
    }
    assert(!queued.result&&read_calls==prior_reads&&write_calls==prior_writes);
    assert(block_unregister_by_id(old_id));
    assert(block_register(&source));assert(block_get_device(0)==dev);
    assert(dev->id!=old_id); /* Same registry slot, new immutable ID. */
    removed_sb.device_id=dev->id; /* Filesystem's saved ID is authoritative. */
    prior_reads=read_calls;prior_writes=write_calls;
    assert(!fat32_mounted_read(&removed_sb,24,1,data));
    assert(!fat32_mounted_write(&removed_sb,24,1,written));
    assert(!exfat_read(&removed_ex,24,1,data));
    assert(!exfat_write(&removed_ex,24,1,written));
    assert(read_calls==prior_reads&&write_calls==prior_writes);
    assert(!mgfs_read_block(dev,old_id,2,data));
    assert(!mgfs_write_block(dev,old_id,2,written));
    logical=physical=count=0;found=false;
    assert(!mgfs_validate_file_extent(&fs,(u8*)extent,&logical,&physical,
                                      &count,3,data,&found));
    assert(!block_read_for_id(dev,old_id,16,8,data));
    assert(!block_write_for_id(dev,old_id,16,8,written));
    assert(read_calls==prior_reads&&write_calls==prior_writes);
    assert(mgfs_read_block(dev,dev->id,2,data));
    run_mounted_filesystem_checks(dev,dev->id,old_id);

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

    /* An admitted flush owns the same gate and can finish while removal
       begins; unregister must wait for that callback to drain. */
    assert(block_register(&source));dev=block_get_device(0);
    hold_callback=true;callback_entered=callback_continue=false;
    read_worker_t active_flush={.thread={.state=THREAD_STATE_RUNNING,
                                         .condition=PTHREAD_COND_INITIALIZER},
                                .dev=dev,.id=dev->id,.kind=3};
    assert(!pthread_create(&thread,NULL,read_worker,&active_flush));
    pthread_mutex_lock(&callback_lock);
    while(!callback_entered)pthread_cond_wait(&callback_cond,&callback_lock);
    pthread_mutex_unlock(&callback_lock);
    assert(block_device_mark_gone(dev->id));
    assert(!block_unregister_by_id(dev->id));
    pthread_mutex_lock(&callback_lock);callback_continue=true;
    pthread_cond_broadcast(&callback_cond);pthread_mutex_unlock(&callback_lock);
    assert(!pthread_join(thread,NULL));assert(!active_flush.result);
    hold_callback=false;assert(block_unregister_by_id(dev->id));

    /* Production VFS lock entry rejects a caller that wakes after loss. */
    vfs_super_t sb={0};mutex_init(&sb.operation_lock);
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
    puts("Filesystem identity: bound FAT32/exFAT/MGFS I/O, slot reuse, failures, removal races and VFS wake checks passed");
}
'''

source = (pre + types + header + fixture + wake + mutex + block +
          vfs_fixture + fat32_view +
          "\n".join(function(mgfs, name) for name in
                    ("mgfs_get_le64", "mgfs_set_error", "mgfs_read_block",
                     "mgfs_write_block", "mgfs_validate_file_extent")) +
          "\n".join(function(fat32, name) for name in
                    ("fat32_mounted_read", "fat32_mounted_write",
                     "fat32_get_next_cluster_view", "fat32_get_next_cluster", "fat32_sync")) +
          "\n".join(function(exfat, name) for name in
                    ("exfat_read", "exfat_write", "exfat_sync")) +
          "\n#undef __atomic_store_n\n" + function(vfs, "vfs_super_operation_lock") +
          function(vfs, "vfs_node_operation_begin") + test)
with tempfile.TemporaryDirectory(prefix="mangrove-mgfs-identity-") as directory:
    path = Path(directory) / "test.c"
    exe = Path(directory) / "test"
    path.write_text(source)
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-fsanitize=address,undefined", str(path), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
